/**
 * useDebouncedQuery
 *
 * A React hook that debounces a query string and fetches data from a URL,
 * with full race-condition safety and clean unmount behaviour.
 *
 * Usage:
 * -------
 * ```tsx
 * function SearchResults() {
 *   const [query, setQuery] = React.useState('');
 *
 *   const { data, loading, error, refetch } = useDebouncedQuery<User[]>(
 *     query,
 *     (q, signal) => fetch(`/api/users?q=${encodeURIComponent(q)}`, { signal })
 *       .then(res => {
 *         if (!res.ok) throw new Error(res.statusText);
 *         return res.json() as Promise<User[]>;
 *       }),
 *     { delay: 300, minLength: 2 }
 *   );
 *
 *   return (
 *     <div>
 *       <input value={query} onChange={e => setQuery(e.target.value)} />
 *       {loading && <p>Loading…</p>}
 *       {error && <p>Error: {error.message}</p>}
 *       {data?.map(u => <p key={u.id}>{u.name}</p>)}
 *       <button onClick={refetch}>Refresh</button>
 *     </div>
 *   );
 * }
 * ```
 *
 * Options:
 *   delay      – debounce wait in ms (default: 300)
 *   minLength  – skip fetch when query is shorter than this (default: 0)
 *
 * Race-condition strategy:
 *   Every call to the fetcher receives a fresh AbortSignal. When a newer
 *   request arrives before the previous one settles, the old AbortController
 *   is aborted. A monotonic request-id counter ensures that even if an aborted
 *   response somehow resolves, stale data is never committed to state.
 *
 * Cleanup:
 *   The debounce timer and in-flight request are both cancelled when the
 *   component unmounts.
 */

import { useState, useEffect, useRef, useCallback } from 'react';

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

export interface UseDebouncedQueryOptions {
  /** Debounce wait in milliseconds. @default 300 */
  delay?: number;
  /**
   * Minimum query length before a fetch is attempted.
   * Queries shorter than this resolve immediately to `data: null`.
   * @default 0
   */
  minLength?: number;
}

export interface UseDebouncedQueryResult<T> {
  data: T | null;
  loading: boolean;
  error: Error | null;
  /** Imperatively re-run the last fetch without changing the query. */
  refetch: () => void;
}

/**
 * Fetcher function supplied by the caller.
 * Receives the debounced query string and an AbortSignal so it can pass the
 * signal directly to fetch() or any other cancellable API.
 */
export type QueryFetcher<T> = (query: string, signal: AbortSignal) => Promise<T>;

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------

export function useDebouncedQuery<T>(
  query: string,
  fetcher: QueryFetcher<T>,
  options: UseDebouncedQueryOptions = {},
): UseDebouncedQueryResult<T> {
  const { delay = 300, minLength = 0 } = options;

  const [data, setData] = useState<T | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<Error | null>(null);

  /**
   * Monotonic counter — incremented before every real fetch.
   * The closure over `requestId` inside each async call lets us detect
   * whether the response belongs to the latest request.
   */
  const requestIdRef = useRef(0);

  /** Holds the AbortController for the currently in-flight request. */
  const controllerRef = useRef<AbortController | null>(null);

  /** Stable ref to the fetcher so the effect dependency array stays minimal. */
  const fetcherRef = useRef<QueryFetcher<T>>(fetcher);
  useEffect(() => {
    fetcherRef.current = fetcher;
  }, [fetcher]);

  // Core fetch logic extracted so both the debounce effect and `refetch` can
  // call it without duplicating code.
  const execute = useCallback((q: string) => {
    // Cancel any previous in-flight request.
    if (controllerRef.current) {
      controllerRef.current.abort();
    }

    const controller = new AbortController();
    controllerRef.current = controller;

    // Grab a fresh id and close over it.
    const id = ++requestIdRef.current;

    setLoading(true);
    setError(null);

    fetcherRef.current(q, controller.signal)
      .then((result) => {
        // Discard stale responses.
        if (id !== requestIdRef.current) return;
        setData(result);
        setError(null);
      })
      .catch((err: unknown) => {
        // AbortError is expected when we cancel; don't surface it as an error.
        if (err instanceof DOMException && err.name === 'AbortError') return;
        if (id !== requestIdRef.current) return;
        setError(err instanceof Error ? err : new Error(String(err)));
        setData(null);
      })
      .finally(() => {
        if (id !== requestIdRef.current) return;
        setLoading(false);
      });
  }, []); // no deps — everything accessed through refs

  // `refetch` re-runs using whatever the latest query value is.
  const latestQueryRef = useRef(query);
  useEffect(() => {
    latestQueryRef.current = query;
  }, [query]);

  const refetch = useCallback(() => {
    const q = latestQueryRef.current;
    if (q.length >= minLength) {
      execute(q);
    }
  }, [execute, minLength]);

  // Debounce effect — runs whenever query or delay changes.
  useEffect(() => {
    if (query.length < minLength) {
      // Query too short: clear results and bail without fetching.
      setData(null);
      setError(null);
      setLoading(false);

      // Cancel any previous in-flight request.
      if (controllerRef.current) {
        controllerRef.current.abort();
        controllerRef.current = null;
      }
      return;
    }

    const timer = setTimeout(() => {
      execute(query);
    }, delay);

    return () => {
      clearTimeout(timer);
    };
  }, [query, delay, minLength, execute]);

  // Cleanup on unmount: cancel the debounce timer (handled above by the
  // effect cleanup) and abort any in-flight request.
  useEffect(() => {
    return () => {
      if (controllerRef.current) {
        controllerRef.current.abort();
      }
    };
  }, []);

  return { data, loading, error, refetch };
}
