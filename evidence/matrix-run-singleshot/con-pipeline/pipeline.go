// pipeline.go — Go 1.22 channel pipeline demo
// Stages: generator → fan-out (N transformers) → fan-in
// Features: context cancellation, bounded buffers, errgroup-free error collection
package main

import (
	"context"
	"errors"
	"fmt"
	"math/rand/v2"
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
// Emits integers 0..count-1 onto a buffered channel.
// Closes the channel (and stops early) when ctx is cancelled.
// ----------------------------------------------------------------------------

func generator(ctx context.Context, count int, bufSize int) <-chan int {
	out := make(chan int, bufSize)
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

// ----------------------------------------------------------------------------
// Stage 2 — Transformer (one worker)
// Reads from in, applies transform, writes Result to out.
// Returns early on ctx cancellation; the caller fans-out N of these.
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
			res := transform(id, v)
			select {
			case <-ctx.Done():
				return
			case out <- res:
			}
		}
	}
}

// transform is the per-item work. It simulates occasional errors and latency.
func transform(workerID, v int) Result {
	// Simulate variable processing time.
	time.Sleep(time.Duration(rand.IntN(20)) * time.Millisecond)

	// Inject a synthetic error ~10% of the time.
	if rand.IntN(10) == 0 {
		return Result{Err: fmt.Errorf("worker %d: failed on value %d", workerID, v)}
	}
	return Result{Value: v * v} // squaring as a stand-in for real work
}

// ----------------------------------------------------------------------------
// Stage 3 — Fan-out + Fan-in
// Spins up n transformers that all read from the same input channel,
// collects their outputs into a single merged channel, and closes it
// once every worker finishes (via WaitGroup).
// ----------------------------------------------------------------------------

func fanOutFanIn(ctx context.Context, n int, in <-chan int, bufSize int) <-chan Result {
	merged := make(chan Result, bufSize)

	var wg sync.WaitGroup
	wg.Add(n)

	for id := range n {
		go func(workerID int) {
			defer wg.Done()
			transformer(ctx, workerID, in, merged)
		}(id)
	}

	// Closer: wait for all workers, then seal the merged channel.
	go func() {
		wg.Wait()
		close(merged)
	}()

	return merged
}

// ----------------------------------------------------------------------------
// Error collection — errgroup-free
// Drains the results channel; collects errors into a slice; returns all values
// and all errors after the channel closes or ctx fires.
// ----------------------------------------------------------------------------

func collect(ctx context.Context, results <-chan Result) ([]int, []error) {
	var values []int
	var errs []error

	for {
		select {
		case <-ctx.Done():
			errs = append(errs, fmt.Errorf("collect: context cancelled: %w", ctx.Err()))
			return values, errs
		case r, ok := <-results:
			if !ok {
				// Channel drained cleanly.
				return values, errs
			}
			if r.Err != nil {
				errs = append(errs, r.Err)
			} else {
				values = append(values, r.Value)
			}
		}
	}
}

// ----------------------------------------------------------------------------
// main — wires everything together with a 500 ms timeout
// ----------------------------------------------------------------------------

func main() {
	const (
		itemCount  = 40  // total integers to generate
		numWorkers = 4   // fan-out width
		bufSize    = 8   // channel buffer depth (bounded)
		timeoutMs  = 500 // overall deadline in milliseconds
	)

	ctx, cancel := context.WithTimeout(context.Background(), timeoutMs*time.Millisecond)
	defer cancel()

	// Stage 1: generate.
	nums := generator(ctx, itemCount, bufSize)

	// Stage 2+3: fan-out to N transformers, fan-in to one result channel.
	results := fanOutFanIn(ctx, numWorkers, nums, bufSize)

	// Collect.
	values, errs := collect(ctx, results)

	// Report.
	fmt.Printf("processed %d items successfully\n", len(values))
	if len(errs) > 0 {
		fmt.Printf("%d error(s):\n", len(errs))
		for _, e := range errs {
			// Don't print context-cancellation errors as failures; they're expected
			// when the timeout fires before the pipeline drains.
			if errors.Is(e, context.DeadlineExceeded) || errors.Is(e, context.Canceled) {
				fmt.Printf("  [timeout] %v\n", e)
			} else {
				fmt.Printf("  [worker]  %v\n", e)
			}
		}
	}

	// Surface the final context state so callers can distinguish
	// a clean drain from a timeout-driven early exit.
	if err := ctx.Err(); err != nil {
		fmt.Printf("pipeline stopped early: %v\n", err)
	} else {
		fmt.Println("pipeline drained cleanly within deadline")
	}
}
