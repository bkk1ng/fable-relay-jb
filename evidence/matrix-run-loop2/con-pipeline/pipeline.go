// pipeline.go — Go 1.22 channel pipeline demo
// Stages: generator → fan-out (N transformers) → fan-in → drain
// Context cancellation propagates to every stage.
// Bounded (buffered) channels throughout.
// Errors collected via a dedicated error channel; no errgroup dependency.

package main

import (
	"context"
	"errors"
	"fmt"
	"math/rand"
	"sync"
	"time"
)

// ----------------------------------------------------------------------------
// Types
// ----------------------------------------------------------------------------

// Result carries either a transformed value or an error from a worker stage.
type Result struct {
	Value int
	Err   error
}

// ----------------------------------------------------------------------------
// Stage 1 — Generator
// Emits integers [0, n) onto out, respecting ctx cancellation.
// out is closed when the generator finishes or ctx is done.
// ----------------------------------------------------------------------------

func generator(ctx context.Context, n int, bufSize int) <-chan int {
	out := make(chan int, bufSize)
	go func() {
		defer close(out)
		for i := range n {
			select {
			case <-ctx.Done():
				return
			case out <- i:
			}
		}
	}()
	return out
}

// ----------------------------------------------------------------------------
// Stage 2 — Transformer (one worker)
// Reads from in, applies work(), writes Result to out.
// Returns when in is closed or ctx is done.
// ----------------------------------------------------------------------------

func transformer(ctx context.Context, id int, in <-chan int, out chan<- Result) {
	for {
		select {
		case <-ctx.Done():
			return
		case v, ok := <-in:
			if !ok {
				return
			}
			res := work(id, v)
			select {
			case <-ctx.Done():
				return
			case out <- res:
			}
		}
	}
}

// work simulates fallible processing: sleeps briefly, fails ~10 % of the time.
func work(workerID, v int) Result {
	time.Sleep(time.Duration(rand.Intn(5)+1) * time.Millisecond)
	if rand.Intn(10) == 0 {
		return Result{Err: fmt.Errorf("worker %d: failed on input %d", workerID, v)}
	}
	return Result{Value: v * v}
}

// ----------------------------------------------------------------------------
// Stage 2 — Fan-out
// Launches n transformer goroutines that all read from the same in channel.
// Returns a merged results channel and a WaitGroup so the caller can wait.
// ----------------------------------------------------------------------------

func fanOut(ctx context.Context, n int, in <-chan int, bufSize int) (<-chan Result, *sync.WaitGroup) {
	out := make(chan Result, bufSize)
	var wg sync.WaitGroup
	wg.Add(n)

	for id := range n {
		id := id // Go 1.22 loop-variable capture is safe, but be explicit
		go func() {
			defer wg.Done()
			transformer(ctx, id, in, out)
		}()
	}

	// Close out once every transformer has exited.
	go func() {
		wg.Wait()
		close(out)
	}()

	return out, &wg
}

// ----------------------------------------------------------------------------
// Stage 3 — Fan-in (merge)
// Splits a single Results channel into separate value and error channels.
// Both are closed when results is drained.
// ----------------------------------------------------------------------------

func fanIn(ctx context.Context, results <-chan Result, bufSize int) (<-chan int, <-chan error) {
	values := make(chan int, bufSize)
	errs := make(chan error, bufSize)

	go func() {
		defer close(values)
		defer close(errs)
		for {
			select {
			case <-ctx.Done():
				return
			case r, ok := <-results:
				if !ok {
					return
				}
				if r.Err != nil {
					select {
					case errs <- r.Err:
					case <-ctx.Done():
						return
					}
				} else {
					select {
					case values <- r.Value:
					case <-ctx.Done():
						return
					}
				}
			}
		}
	}()

	return values, errs
}

// ----------------------------------------------------------------------------
// Error collector — errgroup-free
// Drains errs until it closes; returns all collected errors.
// ----------------------------------------------------------------------------

func collectErrors(errs <-chan error) []error {
	var collected []error
	for e := range errs {
		collected = append(collected, e)
	}
	return collected
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

func main() {
	const (
		itemCount  = 40   // total integers to process
		workers    = 4    // fan-out width
		bufSize    = 8    // channel buffer depth (applies to every stage)
		timeoutSec = 3    // pipeline deadline in seconds
	)

	ctx, cancel := context.WithTimeout(context.Background(), timeoutSec*time.Second)
	defer cancel()

	// Stage 1 — generate
	genCh := generator(ctx, itemCount, bufSize)

	// Stage 2 — fan-out to N transformers
	resultsCh, _ := fanOut(ctx, workers, genCh, bufSize)

	// Stage 3 — fan-in: split values from errors
	valuesCh, errsCh := fanIn(ctx, resultsCh, bufSize)

	// Collect errors concurrently so neither values nor errs blocks.
	var errWg sync.WaitGroup
	errWg.Add(1)
	var pipelineErrs []error
	go func() {
		defer errWg.Done()
		pipelineErrs = collectErrors(errsCh)
	}()

	// Drain values in the main goroutine.
	var sum int
	var count int
	for v := range valuesCh {
		sum += v
		count++
	}

	// Wait for the error collector to finish before reading pipelineErrs.
	errWg.Wait()

	// Report context outcome.
	ctxErr := ctx.Err()
	switch {
	case errors.Is(ctxErr, context.DeadlineExceeded):
		fmt.Println("pipeline: deadline exceeded — partial results")
	case errors.Is(ctxErr, context.Canceled):
		fmt.Println("pipeline: cancelled")
	default:
		fmt.Println("pipeline: completed normally")
	}

	fmt.Printf("values received : %d\n", count)
	fmt.Printf("sum             : %d\n", sum)
	fmt.Printf("errors collected: %d\n", len(pipelineErrs))
	for _, e := range pipelineErrs {
		fmt.Printf("  error: %v\n", e)
	}
}
