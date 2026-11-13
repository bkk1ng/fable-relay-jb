// dirhash — walk a directory, SHA-256 every regular file, emit a sorted manifest.
//
// Usage:
//
//	dirhash [flags] <directory>
//
// Flags:
//
//	--out <file>        write manifest here instead of stdout
//	--jobs <n>          parallel hash workers (default: logical CPUs)
//	--exclude <glob>    skip files whose name or relative path matches glob
//	                    (repeatable; uses filepath.Match syntax)
//
// Manifest format (tab-separated, one file per line, sorted by path):
//
//	<rel-path>\t<size-bytes>\t<sha256-hex>
//
// Summary is printed to stderr.
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
)

// multiFlag lets --exclude be supplied more than once.
type multiFlag []string

func (m *multiFlag) String() string { return strings.Join(*m, ", ") }
func (m *multiFlag) Set(v string) error {
	*m = append(*m, v)
	return nil
}

// entry holds the result of hashing one file.
type entry struct {
	rel  string // path relative to root
	size int64
	sum  string // hex-encoded SHA-256
	err  error
}

// workItem is sent to hash workers.
type workItem struct {
	abs string // absolute (or as-given) path, used for open
	rel string // relative path, stored in manifest
	size int64
}

func main() {
	var excludes multiFlag
	flag.Var(&excludes, "exclude", "glob pattern to exclude (repeatable)")
	jobs := flag.Int("jobs", runtime.NumCPU(), "parallel hash workers")
	outPath := flag.String("out", "", "write manifest to file instead of stdout")

	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s [flags] <directory>\n\nFlags:\n", os.Args[0])
		flag.PrintDefaults()
	}
	flag.Parse()

	if flag.NArg() != 1 {
		flag.Usage()
		os.Exit(2)
	}
	root := flag.Arg(0)

	if *jobs < 1 {
		*jobs = 1
	}

	// Validate exclude patterns early so we can report bad globs before walking.
	for _, pat := range excludes {
		if _, err := filepath.Match(pat, "probe"); err != nil {
			fmt.Fprintf(os.Stderr, "error: invalid --exclude pattern %q: %v\n", pat, err)
			os.Exit(2)
		}
	}

	work := make(chan workItem, *jobs*4)
	results := make(chan entry, *jobs*4)

	// Fan-out: start bounded worker pool.
	var wg sync.WaitGroup
	for i := 0; i < *jobs; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for item := range work {
				sum, err := hashFile(item.abs)
				results <- entry{rel: item.rel, size: item.size, sum: sum, err: err}
			}
		}()
	}
	// Close results when all workers finish.
	go func() {
		wg.Wait()
		close(results)
	}()

	// Walk in its own goroutine so it doesn't block the collector.
	go func() {
		defer close(work)

		walkErr := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
			if err != nil {
				// Warn but keep going; a single unreadable entry shouldn't abort everything.
				fmt.Fprintf(os.Stderr, "warning: skipping %s: %v\n", path, err)
				return nil
			}

			// Skip the .git directory tree entirely.
			if d.IsDir() && d.Name() == ".git" {
				return filepath.SkipDir
			}

			// Skip symlinks (both files and directory symlinks).
			if d.Type()&fs.ModeSymlink != 0 {
				return nil
			}

			// Only hash regular files.
			if !d.Type().IsRegular() {
				return nil
			}

			rel, relErr := filepath.Rel(root, path)
			if relErr != nil {
				rel = path // fall back to the raw path
			}

			// Apply exclude patterns against both the base name and the
			// relative path, so callers can write "*.log" or "build/*.o".
			for _, pat := range excludes {
				if matchGlob(pat, d.Name()) || matchGlob(pat, rel) {
					return nil
				}
			}

			info, infoErr := d.Info()
			if infoErr != nil {
				fmt.Fprintf(os.Stderr, "warning: stat %s: %v\n", path, infoErr)
				return nil
			}

			work <- workItem{abs: path, rel: rel, size: info.Size()}
			return nil
		})

		if walkErr != nil {
			fmt.Fprintf(os.Stderr, "error: walk failed: %v\n", walkErr)
		}
	}()

	// Collect results.
	var entries []entry
	var errCount int

	for e := range results {
		if e.err != nil {
			fmt.Fprintf(os.Stderr, "warning: hashing %s: %v\n", e.rel, e.err)
			errCount++
			continue
		}
		entries = append(entries, e)
	}

	// Sort manifest by relative path for deterministic output.
	sort.Slice(entries, func(i, j int) bool {
		return entries[i].rel < entries[j].rel
	})

	// Open output destination.
	var out io.Writer = os.Stdout
	if *outPath != "" {
		f, createErr := os.Create(*outPath)
		if createErr != nil {
			fmt.Fprintf(os.Stderr, "error: cannot create %s: %v\n", *outPath, createErr)
			os.Exit(1)
		}
		defer f.Close()
		out = f
	}

	// Write manifest.
	var totalSize int64
	for _, e := range entries {
		fmt.Fprintf(out, "%s\t%d\t%s\n", e.rel, e.size, e.sum)
		totalSize += e.size
	}

	// Summary to stderr so it never contaminates the manifest on stdout.
	fmt.Fprintf(os.Stderr, "scanned: %d files, %d bytes total, %d errors\n",
		len(entries), totalSize, errCount)

	if errCount > 0 {
		os.Exit(1)
	}
}

// hashFile returns the hex-encoded SHA-256 digest of the named file.
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

// matchGlob wraps filepath.Match and silently treats malformed patterns as
// non-matching (they were validated before the walk, so this is a safeguard).
func matchGlob(pattern, name string) bool {
	ok, err := filepath.Match(pattern, name)
	return err == nil && ok
}
