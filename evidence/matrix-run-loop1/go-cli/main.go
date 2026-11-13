// main.go — sha256 manifest generator
// Go 1.22, standard library only
//
// Usage:
//
//	go run . [dir]
//	go run . [dir] --out manifest.txt
//	go run . [dir] --exclude '*.tmp' --exclude 'vendor/*' --jobs 8
//
// Walks a directory tree, computes SHA-256 for each regular file (skipping
// symlinks and .git), and writes a path-sorted manifest
// (path<TAB>size<TAB>hash) to stdout or --out. A summary is printed to stderr.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// --- flag helpers ---

// stringSliceValue lets a flag accept repeated --flag value arguments.
type stringSliceValue struct {
	values *[]string
}

func (s stringSliceValue) String() string {
	if s.values == nil || len(*s.values) == 0 {
		return ""
	}
	return strings.Join(*s.values, ",")
}

func (s stringSliceValue) Set(v string) error {
	*s.values = append(*s.values, v)
	return nil
}

// --- task / result ---

type fileTask struct {
	path    string // full filesystem path for opening
	display string // relative path for the manifest
}

type fileResult struct {
	path string // relative path for the manifest
	size int64
	hash string
	err  error
}

func main() {
	var excludes []string

	out := flag.String("out", "", "write manifest to this file instead of stdout")
	jobs := flag.Int("jobs", runtime.NumCPU(), "number of hash worker goroutines")
	flag.Var(stringSliceValue{values: &excludes}, "exclude",
		"glob pattern to exclude (matched against base name and relative path; repeatable)")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: %s [dir] [--out FILE] [--exclude GLOB ...] [--jobs N]\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "Walks a directory, computes SHA-256 per file, writes a sorted manifest.\n\n")
		flag.PrintDefaults()
	}
	flag.Parse()

	root := "."
	if flag.NArg() > 0 {
		root = flag.Arg(0)
	}

	if *jobs < 1 {
		*jobs = 1
	}

	absRoot, err := filepath.Abs(root)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	start := time.Now()

	tasks := make(chan fileTask, *jobs*4)
	results := make(chan fileResult, *jobs*4)

	// --- worker pool ---
	var wg sync.WaitGroup
	for i := 0; i < *jobs; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			h := sha256.New()
			for t := range tasks {
				f, err := os.Open(t.path)
				if err != nil {
					results <- fileResult{
						path: t.display,
						err:  fmt.Errorf("%s: %w", t.path, err),
					}
					continue
				}
				h.Reset()
				n, err := io.Copy(h, f)
				f.Close()
				if err != nil {
					results <- fileResult{
						path: t.display,
						err:  fmt.Errorf("%s: %w", t.path, err),
					}
					continue
				}
				results <- fileResult{
					path: t.display,
					size: n,
					hash: hex.EncodeToString(h.Sum(nil)),
				}
			}
		}()
	}

	// Close results after all workers finish.
	go func() {
		wg.Wait()
		close(results)
	}()

	// --- walker ---
	var excludedCount atomic.Int64

	walkErr := make(chan error, 1)
	go func() {
		err := filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
			if err != nil {
				fmt.Fprintf(os.Stderr, "walk warning: %s: %v\n", path, err)
				return nil
			}

			// Skip symlinks — WalkDir already doesn't follow them, but we
			// also skip the symlink entries themselves.
			if d.Type()&fs.ModeSymlink != 0 {
				return nil
			}

			// Skip .git directories.
			if d.IsDir() && d.Name() == ".git" {
				return fs.SkipDir
			}

			// User exclude patterns — match against base name and relative path.
			rel, _ := filepath.Rel(root, path)
			rel = filepath.ToSlash(rel)
			if isExcluded(rel, d.Name(), excludes) {
				excludedCount.Add(1)
				if d.IsDir() {
					return fs.SkipDir
				}
				return nil
			}

			// Only hash regular files.
			if !d.Type().IsRegular() {
				return nil
			}

			tasks <- fileTask{path: path, display: rel}
			return nil
		})
		close(tasks)
		walkErr <- err
	}()

	// --- collect results ---
	var manifest []fileResult
	var hashErrors int
	for r := range results {
		if r.err != nil {
			fmt.Fprintf(os.Stderr, "hash error: %v\n", r.err)
			hashErrors++
			continue
		}
		manifest = append(manifest, r)
	}

	if e := <-walkErr; e != nil {
		fmt.Fprintf(os.Stderr, "walk error: %v\n", e)
		os.Exit(1)
	}

	// --- sort by path ---
	sort.Slice(manifest, func(i, j int) bool {
		return manifest[i].path < manifest[j].path
	})

	// --- write manifest ---
	var w io.Writer
	if *out != "" {
		f, err := os.Create(*out)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			os.Exit(1)
		}
		defer f.Close()
		w = f
	} else {
		w = os.Stdout
	}

	var totalSize int64
	for _, r := range manifest {
		fmt.Fprintf(w, "%s\t%d\t%s\n", r.path, r.size, r.hash)
		totalSize += r.size
	}

	// --- summary to stderr ---
	elapsed := time.Since(start).Round(time.Millisecond)
	exc := excludedCount.Load()
	fmt.Fprintf(os.Stderr, "%d files, %s, %d errors, %d excluded, %s — %s\n",
		len(manifest), humanSize(totalSize), hashErrors, exc, elapsed, absRoot)
}

// isExcluded checks whether a path matches any of the glob patterns.
// Each pattern is tested against both the base name and the relative path.
func isExcluded(rel, name string, patterns []string) bool {
	if len(patterns) == 0 {
		return false
	}
	for _, p := range patterns {
		if m, _ := filepath.Match(p, name); m {
			return true
		}
		if m, _ := filepath.Match(p, rel); m {
			return true
		}
	}
	return false
}

// humanSize formats a byte count with binary suffixes.
func humanSize(n int64) string {
	if n < 1024 {
		return fmt.Sprintf("%d B", n)
	}
	suffixes := []string{"KiB", "MiB", "GiB", "TiB", "PiB"}
	div, exp := int64(1024), 0
	for s := n / 1024; s >= 1024 && exp < len(suffixes)-1; s /= 1024 {
		div *= 1024
		exp++
	}
	return fmt.Sprintf("%.1f %s", float64(n)/float64(div), suffixes[exp])
}
