// manifest hashes every regular file under a directory tree and writes a
// sorted, tab-delimited manifest (path, size, sha256) to stdout or --out.
// Symlinks and .git directories are always skipped; --exclude accepts repeatable
// glob patterns matched against base names and relative paths.
//
// Usage:
//
//	manifest [options] [directory]
//
// Build:
//
//	go mod init manifest && go build -o manifest main.go
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
	"time"
)

// multiFlag lets a single flag appear multiple times on the command line.
type multiFlag []string

func (m *multiFlag) String() string { return strings.Join(*m, ", ") }
func (m *multiFlag) Set(v string) error {
	*m = append(*m, v)
	return nil
}

// entry holds the result of hashing one file.
type entry struct {
	path string // relative path from the scanned root
	size int64
	hash string // hex-encoded sha256
	err  error  // set if the file could not be read
}

// isExcluded reports whether relPath matches any glob pattern. Each pattern is
// tested against both the base name and the full relative path so that patterns
// like "node_modules" or "*.tmp" work regardless of depth, while patterns like
// "src/test/*" can target a specific subtree.
func isExcluded(relPath string, patterns []string) bool {
	base := filepath.Base(relPath)
	for _, p := range patterns {
		if matched, _ := filepath.Match(p, base); matched {
			return true
		}
		if matched, _ := filepath.Match(p, relPath); matched {
			return true
		}
	}
	return false
}

// hashFile opens full, streams it through sha256, and returns an entry with the
// digest and byte count. relPath is stored as the manifest path.
func hashFile(full, relPath string) entry {
	f, err := os.Open(full)
	if err != nil {
		return entry{path: relPath, err: err}
	}
	defer f.Close()

	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return entry{path: relPath, err: err}
	}
	return entry{
		path: relPath,
		size: n,
		hash: hex.EncodeToString(h.Sum(nil)),
	}
}

func main() {
	var (
		outPath string
		jobs    int
		exclude multiFlag
	)

	flag.StringVar(&outPath, "out", "", "write manifest to this file instead of stdout")
	flag.IntVar(&jobs, "jobs", runtime.NumCPU(), "number of hash worker goroutines")
	flag.Var(&exclude, "exclude", "glob pattern to exclude (base name or relative path; repeatable)")
	flag.Usage = func() {
		fmt.Fprintf(flag.CommandLine.Output(), "usage: %s [options] [directory]\n", os.Args[0])
		flag.PrintDefaults()
	}
	flag.Parse()

	dir := "."
	if flag.NArg() > 0 {
		dir = flag.Arg(0)
	}
	if jobs < 1 {
		jobs = 1
	}

	// validate the target directory
	info, err := os.Stat(dir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	if !info.IsDir() {
		fmt.Fprintf(os.Stderr, "error: %s is not a directory\n", dir)
		os.Exit(1)
	}

	// resolve to absolute so relative paths are stable regardless of cwd
	absDir, err := filepath.Abs(dir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	start := time.Now()

	// task channel fed by the walker; results come back from the worker pool.
	type task struct {
		full    string
		relPath string
	}
	tasks := make(chan task, jobs*2)
	results := make(chan entry, jobs*2)

	// bounded worker pool
	var wg sync.WaitGroup
	for i := 0; i < jobs; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for t := range tasks {
				results <- hashFile(t.full, t.relPath)
			}
		}()
	}

	// collector goroutine gathers results into a slice for sorting.
	var entries []entry
	collectorDone := make(chan struct{})
	go func() {
		for e := range results {
			entries = append(entries, e)
		}
		close(collectorDone)
	}()

	// walk the tree, filtering and dispatching regular files to workers
	walkErr := filepath.WalkDir(absDir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			fmt.Fprintf(os.Stderr, "warning: %v\n", err)
			if d != nil && d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}

		relPath, rerr := filepath.Rel(absDir, path)
		if rerr != nil {
			relPath = path
		}

		// never follow symlinks
		if d.Type()&fs.ModeSymlink != 0 {
			return nil
		}

		// always skip .git directories
		if d.IsDir() && d.Name() == ".git" {
			return filepath.SkipDir
		}

		// apply user exclude patterns
		if len(exclude) > 0 && isExcluded(relPath, exclude) {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}

		// only hash regular files; skip dirs, pipes, sockets, devices, etc.
		if !d.Type().IsRegular() {
			return nil
		}

		tasks <- task{full: path, relPath: relPath}
		return nil
	})

	close(tasks)
	wg.Wait()
	close(results)
	<-collectorDone

	if walkErr != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", walkErr)
		os.Exit(1)
	}

	// sort by relative path for a stable manifest
	sort.Slice(entries, func(i, j int) bool {
		return entries[i].path < entries[j].path
	})

	// write manifest to stdout or --out
	var out io.Writer = os.Stdout
	if outPath != "" {
		f, err := os.Create(outPath)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			os.Exit(1)
		}
		defer f.Close()
		out = f
	}

	var totalSize int64
	var errorCount int
	for _, e := range entries {
		if e.err != nil {
			errorCount++
			fmt.Fprintf(os.Stderr, "warning: %s: %v\n", e.path, e.err)
			continue
		}
		totalSize += e.size
		fmt.Fprintf(out, "%s\t%d\t%s\n", e.path, e.size, e.hash)
	}

	elapsed := time.Since(start)
	fmt.Fprintf(os.Stderr, "%d files, %d bytes, %d errors, %d workers, %v\n",
		len(entries)-errorCount, totalSize, errorCount, jobs, elapsed)
}
