// pipeline.go — channel pipeline: generator → fan-out → fan-in
// Go 1.22 | context cancellation, bounded buffers, errgroup-free error collection
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

// Item is the unit flowing through the pipeline.
type Item struct {
	ID    int
	Value int
}

// Result carries a transformed item or an error from a worker stage.
type Result struct {
	Item Item
	Err  error
}

// ----------------------------------------------------------------------------
// Stage 1 — Generator
// Emits Items until count is reached or ctx is cancelled.
// The returned channel is closed when the stage exits.
// ----------------------------------------------------------------------------

func generate(ctx context.Context, count int) <-chan Item {
	out := make(chan Item, 16) // bounded buffer
	go func() {
		defer close(out)
		for i := range count {
			item := Item{ID: i, Value: rand.IntN(100)}
			select {
			case <-ctx.Done():
				return
			case out <- item:
			}
		}
	}()
	return out
}

// ----------------------------------------------------------------------------
// Stage 2 — Transformer (single worker)
// Reads from in, applies transform, writes Result to out.
// Propagates ctx cancellation; a transform error is sent as a Result, not fatal.
// ----------------------------------------------------------------------------

func transform(ctx context.Context, id int, in <-chan Item, out chan<- Result) {
	for {
		select {
		case <-ctx.Done():
			return
		case item, ok := <-in:
			if !ok {
				return
			}
			r, err := process(id, item)
			res := Result{Item: r, Err: err}
			select {
			case <-ctx.Done():
				return
			case out <- res:
			}
		}
	}
}

// process is the actual work. Returns an error on bad input to demonstrate
// per-item error propagation without killing the pipeline.
func process(workerID int, item Item) (Item, error) {
	// Simulate work.
	time.Sleep(time.Duration(rand.IntN(5)) * time.Millisecond)

	if item.Value == 42 { // contrived error trigger
		return Item{}, fmt.Errorf("worker %d: rejected item %d (value=42)", workerID, item.ID)
	}
	return Item{ID: item.ID, Value: item.Value * 2}, nil
}

// ----------------------------------------------------------------------------
// Fan-out
// Spins up n transformer goroutines that all read from the same source channel.
// Returns a slice of per-worker result channels so fan-in can drain them all.
// ----------------------------------------------------------------------------

func fanOut(ctx context.Context, n int, src <-chan Item) []<-chan Result {
	outs := make([]<-chan Result, n)
	for i := range n {
		ch := make(chan Result, 8) // bounded per-worker buffer
		outs[i] = ch
		go func(id int, out chan<- Result) {
			defer close(out)
			transform(ctx, id, src, out)
		}(i, ch)
	}
	return outs
}

// ----------------------------------------------------------------------------
// Fan-in (merge)
// Merges n result channels into one. Uses a WaitGroup to close the merged
// channel only after every worker has finished.
// ----------------------------------------------------------------------------

func fanIn(ctx context.Context, ins []<-chan Result) <-chan Result {
	merged := make(chan Result, 32) // bounded merged buffer
	var wg sync.WaitGroup

	forward := func(in <-chan Result) {
		defer wg.Done()
		for {
			select {
			case <-ctx.Done():
				return
			case res, ok := <-in:
				if !ok {
					return
				}
				select {
				case <-ctx.Done():
					return
				case merged <- res:
				}
			}
		}
	}

	wg.Add(len(ins))
	for _, in := range ins {
		go forward(in)
	}

	// Close merged once all forwarders are done.
	go func() {
		wg.Wait()
		close(merged)
	}()

	return merged
}

// ----------------------------------------------------------------------------
// Error collector (errgroup-free)
// Drains the merged channel; accumulates errors, returns processed items and
// all errors seen. Runs synchronously in the caller's goroutine so the caller
// can inspect results before the timeout fires.
// ----------------------------------------------------------------------------

func collect(merged <-chan Result) ([]Item, []error) {
	var items []Item
	var errs []error
	for res := range merged {
		if res.Err != nil {
			errs = append(errs, res.Err)
		} else {
			items = append(items, res.Item)
		}
	}
	return items, errs
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

func main() {
	const (
		itemCount  = 50
		numWorkers = 4
		timeout    = 2 * time.Second
	)

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	// Wire the pipeline.
	src := generate(ctx, itemCount)
	workerChans := fanOut(ctx, numWorkers, src)
	merged := fanIn(ctx, workerChans)
	items, errs := collect(merged)

	// Report.
	fmt.Printf("processed: %d items\n", len(items))
	for _, item := range items {
		fmt.Printf("  id=%-3d value=%d\n", item.ID, item.Value)
	}

	if len(errs) > 0 {
		fmt.Printf("\nerrors (%d):\n", len(errs))
		for _, e := range errs {
			fmt.Printf("  %v\n", e)
		}
	}

	if ctxErr := ctx.Err(); errors.Is(ctxErr, context.DeadlineExceeded) {
		fmt.Println("\npipeline cancelled: deadline exceeded")
	}
}
