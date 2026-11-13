// pipeline.go — channel pipeline: generator → fan-out → fan-in
// Go 1.22 | run: go run pipeline.go
package main

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"
)

// Result carries either a transformed value or an error from a stage.
type Result struct {
	Value int
	Err   error
}

// generator emits integers 0..count-1 into a bounded channel.
// It respects ctx cancellation and closes the channel when done.
func generator(ctx context.Context, count int) <-chan int {
	out := make(chan int, 16)
	go func() {
		defer close(out)
		for i := range count {
			select {
			case <-ctx.Done():
				return
			case out <- i:
			}
		}
	}()
	return out
}

// transform reads from in, applies work, and sends Results to out.
// A sentinel error is injected on a chosen value to exercise the error path.
func transform(ctx context.Context, id int, in <-chan int, out chan<- Result) {
	for {
		select {
		case <-ctx.Done():
			return
		case v, ok := <-in:
			if !ok {
				return
			}
			// Inject a synthetic error for demonstration.
			if v == 7 {
				select {
				case <-ctx.Done():
					return
				case out <- Result{Err: fmt.Errorf("worker %d: bad value %d", id, v)}:
				}
				continue
			}
			// Simulate work.
			transformed := v * v
			select {
			case <-ctx.Done():
				return
			case out <- Result{Value: transformed}:
			}
		}
	}
}

// fanOut spins up n workers that all read from src and write to the
// returned results channel. The results channel is closed once every
// worker has exited.
func fanOut(ctx context.Context, n int, src <-chan int) <-chan Result {
	results := make(chan Result, n*4)
	var wg sync.WaitGroup
	wg.Add(n)
	for i := range n {
		go func(id int) {
			defer wg.Done()
			transform(ctx, id, src, results)
		}(i)
	}
	// Close results when every worker is done.
	go func() {
		wg.Wait()
		close(results)
	}()
	return results
}

// collectErrors drains the results channel, separating values from errors.
// It returns all collected values and all errors seen.
func collectErrors(results <-chan Result) ([]int, []error) {
	var values []int
	var errs []error
	for r := range results {
		if r.Err != nil {
			errs = append(errs, r.Err)
		} else {
			values = append(values, r.Value)
		}
	}
	return values, errs
}

func run(ctx context.Context) error {
	const (
		itemCount  = 20
		numWorkers = 4
	)

	src := generator(ctx, itemCount)
	results := fanOut(ctx, numWorkers, src)
	values, errs := collectErrors(results)

	if ctx.Err() != nil {
		return fmt.Errorf("pipeline cancelled: %w", ctx.Err())
	}

	fmt.Printf("collected %d values\n", len(values))
	for _, e := range errs {
		fmt.Printf("  error: %v\n", e)
	}
	if len(errs) > 0 {
		return errors.Join(errs...)
	}
	return nil
}

func main() {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := run(ctx); err != nil {
		fmt.Printf("pipeline finished with errors: %v\n", err)
	} else {
		fmt.Println("pipeline finished cleanly")
	}
}
