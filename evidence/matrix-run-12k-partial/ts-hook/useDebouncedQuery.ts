/**
 * useDebouncedQuery
 *
 * A hook that debounces a query string, fetches data from a URL, and handles
 * race conditions, in-flight cancellation, and cleanup on unmount.
 *
 * Usage:
 * -----------------------------------------------------------------------
 * interface SearchResult {
 *   id: number;
 *   name: string;
 * }
 *
 * function SearchBox() {
 *   const [query, setQuery] = useState('');
 *
 *   const { data, loading, error, refetch } = useDebouncedQuery<SearchResult[]>(
 *     query,
 *     (q, signal) =>
 *       fetch(`/api/search?q=${encodeURIComponent(q)}`, { signal }).then(r => {
 *         if (!r.ok) throw new Error(`HTTP ${r.status}`);
 *         return r.json() as Promise<SearchResult[]>;
 *       }),
 *     { delay: 300, minLength: 2 },
 *   );
 *
 *   return (
 *     <div>
 *       <input value={query} onChange={e => setQuery(e.target.value)} />
 *       {loading && <p>Loading…</p>}
 *       {error && <p>Error: {error.message}</p>}
 *       {data?.map(item => <p key={item.id}>{item.name}</p>)}
 *       <button onClick={refetch}>Retry</button>
 *     </div>
 *   );
 * }
 * -----------------------------------------------------------------------
 *
 * Parameters:
 *   query      — The raw query string, updated on every keystroke.
 *   fetcher    — Async function (query, AbortSignal) => Promise<T>.
 *                Receives the signal so the caller can forward it to fetch()
 *                or any other cancellable API.
 *   options    — Optional config:
 *                  delay      (ms, default 350) — debounce window.
 *                  minLength  (default 1)        — skip the fetch when the
 *                                                  trimmed query is shorter.
 *
 * Returns:
 *   data     — Last successfully resolved value, or null.
 *   loading  — True while a request is in flight.
 *   error    — The last thrown Error, or null.
 *   refetch  — Re-runs the fetch immediately with the current query,
 *              bypassing the debounce.
 */

import { useState, useEffect, useCallback, useRef } from 'react';

export interface UseDebouncedQueryOptions {
  /** Debounce delay in milliseconds. Defaults to 350. */
  delay?: number;
  /** Minimum trimmed query length before a fetch is attempted. Defaults to 1. */
  minLength?: number;
}

export interface UseDebouncedQueryResult<T> {
  data: T | null;
  loading: boolean;
  error: Error | null;
  refetch: () => void;
}

/**
 * Fetcher signature — receives the query string and an AbortSignal.
 * Throw (or let fetch throw) on failure; the hook catches it.
 */
export type Fetcher<T> = (query: string, signal: AbortSignal) => Promise<T>;

export function useDebouncedQuery<T>(
  query: string,
  fetcher: Fetcher<T>,
  options: UseDebouncedQueryOptions = {},
): UseDebouncedQueryResult<T> {
  const { delay = 350, minLength = 1 } = options;

  const [data, setData]       = useState<T | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError]     = useState<Error | null>(null);

  /**
   * Monotonically increasing request id.  The closure captures its value at
   * call time; when the response arrives we compare it to the ref to detect
   * stale responses from an earlier query.
   */
  const requestIdRef = useRef(0);

  /**
   * Holds the AbortController for the currently in-flight request so we can
   * cancel it when the query changes or the component unmounts.
   */
  const controllerRef = useRef<AbortController | null>(null);

  /** Stable ref for the fetcher so the effect dependency array stays clean. */
  const fetcherRef = useRef<Fetcher<T>>(fetcher);
  useEffect(() => { fetcherRef.current = fetcher; }, [fetcher]);

  // -------------------------------------------------------------------------
  // Core fetch logic, shared by the debounced effect and refetch().
  // -------------------------------------------------------------------------
  const execute = useCallback((q: string) => {
    // Cancel any previous in-flight request.
    controllerRef.current?.abort();

    const trimmed = q.trim();
    if (trimmed.length < minLength) {
      setLoading(false);
      return;
    }

    const controller = new AbortController();
    controllerRef.current = controller;

    // Capture the id for this request.
    const id = ++requestIdRef.current;

    setLoading(true);
    setError(null);

    fetcherRef
      .current(trimmed, controller.signal)
      .then((result) => {
        // Discard if a newer request has already been issued.
        if (id !== requestIdRef.current) return;
        setData(result);
        setError(null);
      })
      .catch((err: unknown) => {
        // Ignore aborts — they are intentional, not errors.
        if (err instanceof DOMException && err.name === 'AbortError') return;
        if (id !== requestIdRef.current) return;
        setError(err instanceof Error ? err : new Error(String(err)));
      })
      .finally(() => {
        if (id !== requestIdRef.current) return;
        setLoading(false);
      });
  }, [minLength]);

  // -------------------------------------------------------------------------
  // Debounced effect — fires after `delay` ms of silence on `query`.
  // -------------------------------------------------------------------------
  useEffect(() => {
    const timer = setTimeout(() => execute(query), delay);

    return () => {
      clearTimeout(timer);
    };
  }, [query, delay, execute]);

  // -------------------------------------------------------------------------
  // Abort in-flight request on unmount.
  // -------------------------------------------------------------------------
  useEffect(() => {
    return () => {
      controllerRef.current?.abort();
    };
  }, []);

  // -------------------------------------------------------------------------
  // refetch — bypasses the debounce, useful for retry buttons.
  // -------------------------------------------------------------------------
  const refetch = useCallback(() => {
    execute(query);
  }, [query, execute]);

  return { data, loading, error, refetch };
}
