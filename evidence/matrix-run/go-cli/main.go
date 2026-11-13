// hashdir — walk a directory tree, hash every regular file, emit a sorted manifest.
//
// Usage:
//
//	hashdir [flags] [directory]
//
// Flags:
//
//	--out FILE        write manifest to FILE instead of stdout
//	--jobs N          parallel workers (default: logical CPUs)
//	--exclude GLOB    skip paths whose base name or root-relative path matches GLOB
//	                  (repeatable; uses filepath.Match syntax)
//
// Manifest format (tab-separated, one file per line):
//
//	<path>\t<size-bytes>\t<sha256-hex>
//
// Summary is written to stderr.
package main

import (
	"crypto/sha256"
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

// globList is a repeatable string flag (--exclude can appear multiple times).
type globList []string

func (g *globList) String() string        { return strings.Join(*g, ", ") }
func (g *globList) Set(v string) error    { *g = append(*g, v); return nil }

type entry struct {
	path string // path as returned by WalkDir
	size int64
	hash string
}

func main() {
	var excludes globList
	outFlag := flag.String("out", "", "write manifest to `file` instead of stdout")
	jobsFlag := flag.Int("jobs", runtime.NumCPU(), "number of parallel hash workers")
	flag.Var(&excludes, "exclude", "glob `pattern` to exclude (repeatable, filepath.Match syntax)")
	flag.Parse()

	root := "."
	if flag.NArg() > 0 {
		root = flag.Arg(0)
	}

	nJobs := *jobsFlag
	if nJobs < 1 {
		nJobs = 1
	}

	start := time.Now()

	type workItem struct {
		path string
		rel  string // root-relative path for exclude matching
	}

	work := make(chan workItem, nJobs*8)
	results := make(chan entry, nJobs*8)
	errs := make(chan error, nJobs*8)

	// Workers: receive paths, compute sha256, send entries.
	var wg sync.WaitGroup
	for i := 0; i < nJobs; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for item := range work {
				e, err := hashFile(item.path)
				if err != nil {
					errs <- fmt.Errorf("%s: %w", item.path, err)
					continue
				}
				results <- e
			}
		}()
	}

	// Close results and errs once all workers finish.
	go func() {
		wg.Wait()
		close(results)
		close(errs)
	}()

	// Walk the tree and feed the work channel.
	walkErr := make(chan error, 1)
	go func() {
		defer close(work)
		err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
			if err != nil {
				// Log access errors but keep walking.
				fmt.Fprintf(os.Stderr, "warning: %v\n", err)
				return nil
			}
			// Skip the .git directory tree.
			if d.IsDir() && d.Name() == ".git" {
				return filepath.SkipDir
			}
			// Skip symlinks.
			if d.Type()&fs.ModeSymlink != 0 {
				return nil
			}
			// Only hash regular files.
			if !d.Type().IsRegular() {
				return nil
			}
			// Compute root-relative path for glob matching.
			rel, err := filepath.Rel(root, path)
			if err != nil {
				rel = path
			}
			// Apply exclude patterns against base name and relative path.
			for _, pat := range excludes {
				if ok, _ := filepath.Match(pat, d.Name()); ok {
					return nil
				}
				if ok, _ := filepath.Match(pat, rel); ok {
					return nil
				}
			}
			work <- workItem{path: path, rel: rel}
			return nil
		})
		if err != nil {
			walkErr <- err
		} else {
			close(walkErr)
		}
	}()

	// Collect results and errors (drain both channels until closed).
	var entries []entry
	var errCount int

	resultsDone := false
	errsDone := false
	for !resultsDone || !errsDone {
		select {
		case e, ok := <-results:
			if !ok {
				resultsDone = true
				results = nil
				continue
			}
			entries = append(entries, e)
		case e, ok := <-errs:
			if !ok {
				errsDone = true
				errs = nil
				continue
			}
			fmt.Fprintf(os.Stderr, "error: %v\n", e)
			errCount++
		}
	}

	if err := <-walkErr; err != nil {
		fmt.Fprintf(os.Stderr, "fatal: walk error: %v\n", err)
		os.Exit(1)
	}

	// Sort by path for deterministic output.
	sort.Slice(entries, func(i, j int) bool {
		return entries[i].path < entries[j].path
	})

	// Open the output destination.
	out := io.Writer(os.Stdout)
	if *outFlag != "" {
		f, err := os.Create(*outFlag)
		if err != nil {
			fmt.Fprintf(os.Stderr, "fatal: cannot create output file: %v\n", err)
			os.Exit(1)
		}
		defer f.Close()
		out = f
	}

	// Write manifest: path TAB size TAB sha256-hex
	var totalBytes int64
	for _, e := range entries {
		fmt.Fprintf(out, "%s\t%d\t%s\n", e.path, e.size, e.hash)
		totalBytes += e.size
	}

	// Summary to stderr.
	elapsed := time.Since(start).Round(time.Millisecond)
	fmt.Fprintf(os.Stderr,
		"files: %d  size: %s  errors: %d  time: %s\n",
		len(entries), humanBytes(totalBytes), errCount, elapsed,
	)
}

// hashFile opens path, stats it, and returns its sha256 digest.
func hashFile(path string) (entry, error) {
	f, err := os.Open(path)
	if err != nil {
		return entry{}, err
	}
	defer f.Close()

	info, err := f.Stat()
	if err != nil {
		return entry{}, err
	}
	// Recheck: skip if something changed it to a non-regular file between walk and open.
	if !info.Mode().IsRegular() {
		return entry{}, fmt.Errorf("not a regular file (skipped)")
	}

	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return entry{}, err
	}

	return entry{
		path: path,
		size: info.Size(),
		hash: fmt.Sprintf("%x", h.Sum(nil)),
	}, nil
}

// humanBytes formats b as a human-readable size string.
func humanBytes(b int64) string {
	if b < 1024 {
		return fmt.Sprintf("%d B", b)
	}
	div, exp := int64(1024), 0
	for n := b / 1024; n >= 1024; n /= 1024 {
		div *= 1024
		exp++
	}
	return fmt.Sprintf("%.2f %ciB", float64(b)/float64(div), "KMGTPE"[exp])
}
