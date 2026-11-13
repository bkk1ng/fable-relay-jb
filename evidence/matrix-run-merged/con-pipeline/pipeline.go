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

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// Result carries a transformed value or an error from a single pipeline item.
type Result struct {
	Value int
	Err   error
}

// ---------------------------------------------------------------------------
// Stage 1 — Generator
// Emits integers 0..count-1 onto a buffered channel, then closes it.
// Respects ctx cancellation: stops early and closes the channel cleanly.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Stage 2 — Transformer (single worker)
// Reads from in, applies work(), writes Result to out.
// Stopped by ctx or by in being closed.
// ---------------------------------------------------------------------------

func transformer(ctx context.Context, id int, in <-chan int, out chan<- Result) {
	for {
		select {
		case <-ctx.Done():
			return
		case v, ok := <-in:
			if !ok {
				return
			}
			out <- work(ctx, id, v)
		}
	}
}

// work is the per-item computation. It simulates occasional errors and latency.
func work(ctx context.Context, workerID int, v int) Result {
	// Simulate variable processing time (1–5ms).
	delay := time.Duration(1+rand.IntN(5)) * time.Millisecond
	select {
	case <-ctx.Done():
		return Result{Err: ctx.Err()}
	case <-time.After(delay):
	}

	// Inject a synthetic error ~10% of the time.
	if v%10 == 7 {
		return Result{Err: fmt.Errorf("worker %d: bad value %d", workerID, v)}
	}
	return Result{Value: v * v}
}

// ---------------------------------------------------------------------------
// Fan-out + Fan-in
// fanOut spins up n transformer goroutines, all reading from the same source.
// fanIn merges n Result channels into one, closing it once every worker exits.
// ---------------------------------------------------------------------------

func fanOut(ctx context.Context, n int, source <-chan int, bufSize int) []<-chan Result {
	channels := make([]<-chan Result, n)
	for i := range n {
		ch := make(chan Result, bufSize)
		channels[i] = ch
		go func(id int, out chan<- Result) {
			defer close(out)
			transformer(ctx, id, source, out)
		}(i, ch)
	}
	return channels
}

func fanIn(ctx context.Context, inputs []<-chan Result) <-chan Result {
	merged := make(chan Result, len(inputs))
	var wg sync.WaitGroup

	forward := func(ch <-chan Result) {
		defer wg.Done()
		for {
			select {
			case <-ctx.Done():
				return
			case r, ok := <-ch:
				if !ok {
					return
				}
				select {
				case <-ctx.Done():
					return
				case merged <- r:
				}
			}
		}
	}

	wg.Add(len(inputs))
	for _, ch := range inputs {
		go forward(ch)
	}

	// Close merged once every forwarder has exited.
	go func() {
		wg.Wait()
		close(merged)
	}()

	return merged
}

// ---------------------------------------------------------------------------
// Error collector — errgroup-free
// Collects every non-nil error from the pipeline into a slice.
// Call collect() after the results channel is drained to retrieve them.
// ---------------------------------------------------------------------------

type errorCollector struct {
	mu   sync.Mutex
	errs []error
}

func (ec *errorCollector) add(err error) {
	ec.mu.Lock()
	ec.errs = append(ec.errs, err)
	ec.mu.Unlock()
}

func (ec *errorCollector) collect() []error {
	ec.mu.Lock()
	defer ec.mu.Unlock()
	out := make([]error, len(ec.errs))
	copy(out, ec.errs)
	return out
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

func main() {
	const (
		itemCount  = 50  // total integers to process
		workers    = 4   // fan-out width
		bufSize    = 8   // buffer depth for every channel in the pipeline
		timeoutSec = 3   // overall deadline
	)

	ctx, cancel := context.WithTimeout(context.Background(), timeoutSec*time.Second)
	defer cancel()

	// Build the pipeline.
	source := generator(ctx, itemCount, bufSize)
	workerChans := fanOut(ctx, workers, source, bufSize)
	results := fanIn(ctx, workerChans)

	// Drain results and collect errors.
	var ec errorCollector
	var successes int

	for r := range results {
		if r.Err != nil {
			ec.add(r.Err)
			continue
		}
		successes++
		fmt.Printf("  ok  %d\n", r.Value)
	}

	// Report.
	errs := ec.collect()
	fmt.Printf("\n--- pipeline complete ---\n")
	fmt.Printf("successes : %d\n", successes)
	fmt.Printf("errors    : %d\n", len(errs))
	for _, e := range errs {
		fmt.Printf("  err: %v\n", e)
	}

	if ctxErr := ctx.Err(); errors.Is(ctxErr, context.DeadlineExceeded) {
		fmt.Println("note: pipeline was cut short by timeout")
	}
}
