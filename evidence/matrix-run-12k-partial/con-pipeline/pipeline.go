// pipeline.go — Go 1.22 channel pipeline demo
//
// Stages:
//   generator  → fan-out (N transformers) → fan-in (WaitGroup) → collector
//
// Every stage propagates context cancellation via a select on ctx.Done().
// All channels are buffered (bounded). Errors are collected into a slice
// without errgroup; the error channel is a result field on a shared type.

package main

import (
	"context"
	"fmt"
	"math/rand/v2"
	"sync"
	"time"
)

// result carries either a transformed value or a stage error.
// Using a single channel for both keeps fan-in straightforward.
type result struct {
	value int
	err   error
}

// ─── stage 1: generator ──────────────────────────────────────────────────────

// generator emits integers [0, count) on a bounded channel.
// It stops early if ctx is cancelled and always closes the channel.
func generator(ctx context.Context, count int) <-chan int {
	ch := make(chan int, 64) // bounded: producers block when consumers are slow
	go func() {
		defer close(ch)
		for i := range count { // range-over-int: Go 1.22+
			select {
			case <-ctx.Done():
				return
			case ch <- i:
			}
		}
	}()
	return ch
}

// ─── stage 2: transformer ────────────────────────────────────────────────────

// transformer reads integers from in, squares each value after a small random
// delay, and writes results to out. Every 17th non-zero item is rejected with
// a synthetic error to exercise the error path.
//
// It exits when in is drained and closed or ctx is cancelled — whichever
// comes first.
func transformer(ctx context.Context, workerID int, in <-chan int, out chan<- result) {
	for {
		select {
		case <-ctx.Done():
			return
		case v, ok := <-in:
			if !ok {
				return // upstream closed; this worker is done
			}
			// Simulate variable-duration work.
			time.Sleep(time.Duration(rand.IntN(8)) * time.Millisecond)

			var r result
			if v > 0 && v%17 == 0 {
				r = result{err: fmt.Errorf("worker %d: rejected value %d", workerID, v)}
			} else {
				r = result{value: v * v}
			}

			select {
			case <-ctx.Done():
				return
			case out <- r:
			}
		}
	}
}

// ─── fan-out + fan-in ────────────────────────────────────────────────────────

// pipeline starts n transformer goroutines that all compete for items from src
// (fan-out). A WaitGroup tracks the workers; a closer goroutine waits on it
// and closes the merged channel (fan-in). The returned channel is closed
// exactly once, after the last worker returns.
//
// No errgroup is used — synchronisation is a bare sync.WaitGroup.
func pipeline(ctx context.Context, n int, src <-chan int) <-chan result {
	merged := make(chan result, n*16) // bounded fan-in buffer

	var wg sync.WaitGroup
	wg.Add(n)

	for id := range n { // range-over-int: Go 1.22+
		go func() {
			defer wg.Done()
			transformer(ctx, id, src, merged)
		}()
	}

	// Closer: waits for all workers to finish, then seals the merged channel
	// so the collector's range loop terminates cleanly.
	go func() {
		wg.Wait()
		close(merged)
	}()

	return merged
}

// ─── stage 3: collector ──────────────────────────────────────────────────────

// collect drains the results channel, tallying good values and accumulating
// errors into a slice. It does not use errgroup; it simply ranges over the
// channel until it is closed.
//
// Because the pipeline propagates ctx cancellation, the channel will close
// promptly after the deadline fires even if not all items were processed.
func collect(results <-chan result) (good int, errs []error) {
	for r := range results {
		if r.err != nil {
			errs = append(errs, r.err)
		} else {
			good++
		}
	}
	return
}

// ─── main ────────────────────────────────────────────────────────────────────

func main() {
	const (
		itemCount  = 200
		numWorkers = 6
		timeout    = 1500 * time.Millisecond // short enough to trigger early exit
	)

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	start := time.Now()

	src := generator(ctx, itemCount)
	results := pipeline(ctx, numWorkers, src)
	good, errs := collect(results)

	elapsed := time.Since(start).Round(time.Millisecond)

	fmt.Printf("elapsed:   %v\n", elapsed)
	fmt.Printf("processed: %d items\n", good)
	fmt.Printf("errors:    %d\n", len(errs))
	for _, e := range errs {
		fmt.Printf("  %v\n", e)
	}
	if ctx.Err() != nil {
		fmt.Printf("stopped:   %v\n", ctx.Err())
	} else {
		fmt.Println("stopped:   pipeline drained cleanly")
	}
}
