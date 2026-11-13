/**
 * useDebouncedQuery — debounced async query hook with request cancellation
 * and race-condition handling for React 18.
 *
 * API
 *   const { data, loading, error, refetch } = useDebouncedQuery(
 *     query,                       // string that triggers a fetch after debounce
 *     fetcher,                     // (query, signal) => Promise<T>
 *     debounceMs = 300             // optional, defaults to 300 ms
 *   );
 *
 * What it does
 *   - Debounces `query` by `debounceMs` before calling `fetcher`.
 *   - Cancels in-flight requests with an AbortController when the query changes
 *     or the component unmounts. The AbortSignal is forwarded to `fetcher`
 *     so the underlying network call (fetch, axios, etc.) can be aborted too.
 *   - Guards against race conditions with a monotonically increasing request id:
 *     only the most recent request is allowed to write state, so stale
 *     responses can never overwrite fresh data.
 *   - Clears state immediately when the query is empty (no fetch, no debounce).
 *   - `refetch()` re-runs the fetcher for the current query without debouncing.
 *
 * Usage
 *   type User = { id: number; name: string };
 *
 *   function searchUsers(query: string, signal: AbortSignal): Promise<User[]> {
 *     return fetch(`/api/users?q=${encodeURIComponent(query)}`, { signal })
 *       .then((res) => {
 *         if (!res.ok) throw new Error(`HTTP ${res.status}`);
 *         return res.json() as Promise<User[]>;
 *       });
 *   }
 *
 *   function SearchBox() {
 *     const [query, setQuery] = useState("");
 *     const { data, loading, error, refetch } = useDebouncedQuery(
 *       query,
 *       searchUsers,
 *       250
 *     );
 *
 *     return (
 *       <>
 *         <input value={query} onChange={(e) => setQuery(e.target.value)} />
 *         {loading && <p>Searching…</p>}
 *         {error && <p>Error: {error.message}</p>}
 *         <ul>
 *           {data?.map((u) => <li key={u.id}>{u.name}</li>)}
 *         </ul>
 *         <button onClick={refetch}>Retry</button>
 *       </>
 *     );
 *   }
 *
 * Notes
 *   - The `fetcher` is read through a ref, so changing its identity between
 *     renders does NOT re-trigger the debounce — the next scheduled fetch
 *     (or a `refetch()`) will pick up the latest function automatically.
 *   - The AbortSignal is only useful if `fetcher` actually forwards it to the
 *     underlying transport. If it doesn't, the hook still prevents stale
 *     responses from writing state via the request-id guard.
 */

import { useCallback, useEffect, useRef, useState } from "react";

export interface UseDebouncedQueryResult<T> {
  data: T | null;
  loading: boolean;
  error: Error | null;
  refetch: () => void;
}

/**
 * Debounced query hook.
 *
 * @typeParam T  Type of the data returned by `fetcher`.
 * @param query      The query string. Empty/whitespace clears state without fetching.
 * @param fetcher    Async function that receives the query and an AbortSignal.
 *                   Forward the signal to your transport so requests are truly cancelled.
 * @param debounceMs Debounce delay in milliseconds (default 300).
 */
export function useDebouncedQuery<T>(
  query: string,
  fetcher: (query: string, signal: AbortSignal) => Promise<T>,
  debounceMs = 300,
): UseDebouncedQueryResult<T> {
  const [data, setData] = useState<T | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<Error | null>(null);

  // Mutable refs — survive re-renders without retriggering effects.
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const abortRef = useRef<AbortController | null>(null);
  const requestIdRef = useRef(0);

  // Keep the latest query and fetcher available to the stable `execute` callback
  // without adding them to its dependency array (which would re-create it).
  const queryRef = useRef(query);
  const fetcherRef = useRef(fetcher);
  queryRef.current = query;
  fetcherRef.current = fetcher;

  /**
   * Fire a fetch for the current query, bypassing debounce.
   * Aborts any in-flight request first and tags the new one with a request id
   * so only the latest response can write state.
   */
  const execute = useCallback(() => {
    // Invalidate all previous requests in one step.
    const currentId = ++requestIdRef.current;

    // Abort any request that's still in flight.
    abortRef.current?.abort();

    const controller = new AbortController();
    abortRef.current = controller;

    const currentQuery = queryRef.current;

    // Empty query: reset state and skip the network entirely.
    if (!currentQuery.trim()) {
      setData(null);
      setError(null);
      setLoading(false);
      return;
    }

    setLoading(true);
    setError(null);

    fetcherRef
      .current(currentQuery, controller.signal)
      .then((result) => {
        // Drop the response if a newer request started or this one was aborted.
        if (currentId !== requestIdRef.current || controller.signal.aborted) {
          return;
        }
        setData(result);
        setError(null);
      })
      .catch((err: unknown) => {
        // AbortError or a superseded request: ignore silently.
        if (currentId !== requestIdRef.current || controller.signal.aborted) {
          return;
        }
        setError(err instanceof Error ? err : new Error(String(err)));
      })
      .finally(() => {
        // Only clear loading if this is still the active request.
        if (currentId !== requestIdRef.current) {
          return;
        }
        setLoading(false);
      });
  }, []);

  // Debounce: restart the timer on every query change.
  useEffect(() => {
    queryRef.current = query;

    if (timerRef.current) {
      clearTimeout(timerRef.current);
      timerRef.current = null;
    }

    // Empty query clears immediately — no debounce, no fetch.
    if (!query.trim()) {
      abortRef.current?.abort();
      setData(null);
      setError(null);
      setLoading(false);
      return;
    }

    timerRef.current = setTimeout(() => {
      timerRef.current = null;
      execute();
    }, debounceMs);

    return () => {
      if (timerRef.current) {
        clearTimeout(timerRef.current);
        timerRef.current = null;
      }
    };
  }, [query, debounceMs, execute]);

  // Unmount cleanup: cancel timer, abort request, invalidate responses.
  useEffect(() => {
    return () => {
      if (timerRef.current) {
        clearTimeout(timerRef.current);
        timerRef.current = null;
      }
      abortRef.current?.abort();
      // Prevent any in-flight then/catch/finally from touching state after unmount.
      requestIdRef.current++;
    };
  }, []);

  const refetch = useCallback(() => {
    execute();
  }, [execute]);

  return { data, loading, error, refetch };
}

export default useDebouncedQuery;
