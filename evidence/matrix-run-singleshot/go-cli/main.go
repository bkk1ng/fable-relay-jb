// hashdir computes SHA-256 for every file under a directory tree,
// skipping symlinks and .git, and writes a sorted manifest to stdout
// or a file. Requires Go 1.22+, standard library only.
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
	"slices"
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

// globMatches reports whether name matches any of the provided glob patterns.
// Only the base name is tested against each pattern.
func globMatches(name string, patterns []string) bool {
	base := filepath.Base(name)
	for _, p := range patterns {
		matched, err := filepath.Match(p, base)
		if err == nil && matched {
			return true
		}
		// Also try the full relative path for patterns that contain a separator.
		if strings.ContainsRune(p, filepath.Separator) {
			if matched, err := filepath.Match(p, name); err == nil && matched {
				return true
			}
		}
	}
	return false
}

// hashFile opens path and returns its SHA-256 digest and byte size.
func hashFile(path string) (string, int64, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", 0, err
	}
	defer f.Close()

	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return "", 0, err
	}
	return hex.EncodeToString(h.Sum(nil)), n, nil
}

func main() {
	var (
		outPath  string
		jobs     int
		excludes multiFlag
	)

	flag.StringVar(&outPath, "out", "", "write manifest to `file` instead of stdout")
	flag.IntVar(&jobs, "jobs", runtime.NumCPU(), "number of parallel workers (default: logical CPU count)")
	flag.Var(&excludes, "exclude", "glob pattern to exclude (repeatable, matched against base name)")
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

	if jobs < 1 {
		jobs = 1
	}

	// --- collect paths to hash -------------------------------------------------

	type workItem struct {
		path string
		info os.FileInfo
	}

	var items []workItem

	err := filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			fmt.Fprintf(os.Stderr, "warning: cannot access %q: %v\n", path, err)
			return nil // keep walking
		}

		// Skip .git at any depth.
		if info.IsDir() && info.Name() == ".git" {
			return filepath.SkipDir
		}

		// Skip symlinks.
		if info.Mode()&os.ModeSymlink != 0 {
			return nil
		}

		// Re-stat to resolve through any lstat; Walk uses Lstat, so a symlink
		// to a directory would still appear as a dir after the check above, but
		// we guard again here for safety.
		if !info.Mode().IsRegular() {
			return nil
		}

		// Compute the path relative to root for exclusion matching.
		rel, relErr := filepath.Rel(root, path)
		if relErr != nil {
			rel = path
		}
		if globMatches(rel, excludes) {
			return nil
		}

		items = append(items, workItem{path: path, info: info})
		return nil
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "error walking %q: %v\n", root, err)
		os.Exit(1)
	}

	// --- hash in parallel ------------------------------------------------------

	work := make(chan workItem, jobs*2)
	results := make(chan entry, jobs*2)

	var wg sync.WaitGroup
	for range jobs {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for item := range work {
				h, size, herr := hashFile(item.path)
				results <- entry{path: item.path, size: size, hash: h, err: herr}
			}
		}()
	}

	go func() {
		for _, item := range items {
			work <- item
		}
		close(work)
		wg.Wait()
		close(results)
	}()

	var entries []entry
	var hashErrors []string

	for r := range results {
		if r.err != nil {
			hashErrors = append(hashErrors, fmt.Sprintf("warning: %s: %v", r.path, r.err))
			continue
		}
		// Store relative path in the manifest.
		rel, relErr := filepath.Rel(root, r.path)
		if relErr != nil {
			rel = r.path
		}
		entries = append(entries, entry{path: rel, size: r.size, hash: r.hash})
	}

	// Sort deterministically by path.
	slices.SortFunc(entries, func(a, b entry) int {
		return strings.Compare(a.path, b.path)
	})

	// --- write manifest --------------------------------------------------------

	out := os.Stdout
	if outPath != "" {
		f, createErr := os.Create(outPath)
		if createErr != nil {
			fmt.Fprintf(os.Stderr, "error creating output file: %v\n", createErr)
			os.Exit(1)
		}
		defer f.Close()
		out = f
	}

	tw := tabwriter.NewWriter(out, 0, 0, 2, ' ', 0)
	fmt.Fprintf(tw, "PATH\tSIZE\tSHA256\n")
	for _, e := range entries {
		fmt.Fprintf(tw, "%s\t%d\t%s\n", e.path, e.size, e.hash)
	}
	tw.Flush()

	// --- summary to stderr -----------------------------------------------------

	for _, msg := range hashErrors {
		fmt.Fprintln(os.Stderr, msg)
	}

	var totalSize int64
	for _, e := range entries {
		totalSize += e.size
	}
	fmt.Fprintf(os.Stderr, "summary: %d file(s), %d byte(s) total, %d error(s)\n",
		len(entries), totalSize, len(hashErrors))
}

// multiFlag is a flag.Value that accumulates repeated --exclude values.
type multiFlag []string

func (m *multiFlag) String() string { return strings.Join(*m, ", ") }
func (m *multiFlag) Set(v string) error {
	*m = append(*m, v)
	return nil
}
