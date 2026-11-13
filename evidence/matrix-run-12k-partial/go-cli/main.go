package main

import (
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"text/tabwriter"
)

// entry holds the result of hashing one file.
type entry struct {
	path string
	size int64
	hash string
	err  error
}

// work is a job sent to a worker.
type work struct {
	path string
	size int64
}

func main() {
	var (
		outFlag     = flag.String("out", "", "write manifest to `file` instead of stdout")
		jobsFlag    = flag.Int("jobs", runtime.NumCPU(), "number of parallel workers")
		excludeFlag multiFlag
	)
	flag.Var(&excludeFlag, "exclude", "glob pattern to exclude (may be repeated)")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: hashdir [flags] <directory>\n\n")
		flag.PrintDefaults()
	}
	flag.Parse()

	if flag.NArg() != 1 {
		flag.Usage()
		os.Exit(2)
	}
	root := flag.Arg(0)

	if *jobsFlag < 1 {
		*jobsFlag = 1
	}

	// Collect files to hash.
	jobs, walkErr := collectFiles(root, excludeFlag)
	if walkErr != nil {
		fmt.Fprintf(os.Stderr, "error walking %q: %v\n", root, walkErr)
		os.Exit(1)
	}

	// Fan out to workers.
	workCh := make(chan work, len(jobs))
	for _, j := range jobs {
		workCh <- j
	}
	close(workCh)

	resultCh := make(chan entry, len(jobs))
	var wg sync.WaitGroup
	for i := 0; i < *jobsFlag; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := range workCh {
				h, err := hashFile(j.path)
				resultCh <- entry{path: j.path, size: j.size, hash: h, err: err}
			}
		}()
	}
	go func() {
		wg.Wait()
		close(resultCh)
	}()

	// Gather results.
	var entries []entry
	var errCount int
	for r := range resultCh {
		if r.err != nil {
			fmt.Fprintf(os.Stderr, "error hashing %q: %v\n", r.path, r.err)
			errCount++
			continue
		}
		entries = append(entries, r)
	}

	// Sort by path for a deterministic manifest.
	sort.Slice(entries, func(i, j int) bool {
		return entries[i].path < entries[j].path
	})

	// Open output destination.
	out := os.Stdout
	if *outFlag != "" {
		f, err := os.Create(*outFlag)
		if err != nil {
			fmt.Fprintf(os.Stderr, "cannot create output file: %v\n", err)
			os.Exit(1)
		}
		defer f.Close()
		out = f
	}

	// Write manifest with aligned columns.
	tw := tabwriter.NewWriter(out, 0, 0, 2, ' ', 0)
	for _, e := range entries {
		fmt.Fprintf(tw, "%s\t%d\t%s\n", e.path, e.size, e.hash)
	}
	if err := tw.Flush(); err != nil {
		fmt.Fprintf(os.Stderr, "write error: %v\n", err)
		os.Exit(1)
	}

	// Summary to stderr.
	var totalBytes int64
	for _, e := range entries {
		totalBytes += e.size
	}
	fmt.Fprintf(os.Stderr, "files: %d  bytes: %d  errors: %d\n",
		len(entries), totalBytes, errCount)

	if errCount > 0 {
		os.Exit(1)
	}
}

// collectFiles walks root, skips symlinks, .git directories, and paths
// matching any of the exclude globs, and returns a slice of work items.
func collectFiles(root string, excludes []string) ([]work, error) {
	var jobs []work

	err := filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			// Report access errors but continue walking.
			fmt.Fprintf(os.Stderr, "warning: cannot access %q: %v\n", path, err)
			return nil
		}

		// Skip .git at any depth.
		if d.IsDir() && d.Name() == ".git" {
			return filepath.SkipDir
		}

		// Skip symlinks entirely (both symlinked files and symlinked dirs).
		if d.Type()&os.ModeSymlink != 0 {
			return nil
		}

		if d.IsDir() {
			return nil
		}

		// Apply exclude globs against the base name and the full path.
		for _, pattern := range excludes {
			// Match against the base name first (simple patterns like "*.tmp").
			if matched, _ := filepath.Match(pattern, d.Name()); matched {
				return nil
			}
			// Also match against the full path so callers can write "vendor/**"
			// style patterns using path separators.
			if matched, _ := filepath.Match(pattern, path); matched {
				return nil
			}
		}

		info, err := d.Info()
		if err != nil {
			fmt.Fprintf(os.Stderr, "warning: stat %q: %v\n", path, err)
			return nil
		}

		jobs = append(jobs, work{path: path, size: info.Size()})
		return nil
	})

	return jobs, err
}

// hashFile computes the SHA-256 digest of the file at path.
func hashFile(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()

	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// multiFlag lets --exclude be specified more than once.
type multiFlag []string

func (m *multiFlag) String() string {
	return strings.Join(*m, ", ")
}

func (m *multiFlag) Set(v string) error {
	*m = append(*m, v)
	return nil
}
