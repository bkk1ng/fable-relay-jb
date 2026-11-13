/**
 * useDebouncedQuery
 *
 * A React hook that debounces a query string, fires a fetch with an AbortController,
 * and handles race conditions so only the most recent request's result is applied.
 *
 * Usage:
 * -------
 *   async function searchUsers(query: string, signal: AbortSignal): Promise<User[]> {
 *     const res = await fetch(`/api/users?q=${encodeURIComponent(query)}`, { signal });
 *     if (!res.ok) throw new Error(`HTTP ${res.status}`);
 *     return res.json();
 *   }
 *
 *   function SearchBox() {
 *     const [query, setQuery] = React.useState('');
 *
 *     const { data, loading, error, refetch } = useDebouncedQuery(query, searchUsers, {
 *       debounceMs: 300,
 *       skip: query.trim().length < 2,
 *     });
 *
 *     return (
 *       <>
 *         <input value={query} onChange={e => setQuery(e.target.value)} />
 *         {loading && <span>Searching…</span>}
 *         {error && <span>Error: {error.message}</span>}
 *         {data?.map(u => <div key={u.id}>{u.name}</div>)}
 *         <button onClick={refetch}>Refresh</button>
 *       </>
 *     );
 *   }
 *
 * Options:
 *   debounceMs  — milliseconds to wait after the last query change before firing (default: 300)
 *   skip        — when true the hook sits idle and clears any existing data (default: false)
 *
 * Race-condition safety:
 *   Each invocation increments a module-local request counter. The hook only commits
 *   a result to state when the counter value at call time still matches the current
 *   counter at resolution time, so stale responses from slow in-flight requests are
 *   silently discarded.
 *
 *   An AbortController is created per request and aborted on:
 *     - a new query arriving before the previous request settled
 *     - component unmount
 */

import { useState, useEffect, useCallback, useRef } from 'react';

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

export interface UseDebouncedQueryOptions {
  /** How long to wait (ms) after the last query change before firing. Default: 300 */
  debounceMs?: number;
  /** When true the hook stays idle and returns empty state. Default: false */
  skip?: boolean;
}

export interface UseDebouncedQueryResult<TData> {
  data: TData | null;
  loading: boolean;
  error: Error | null;
  /** Immediately re-runs the last query without waiting for the debounce. */
  refetch: () => void;
}

/**
 * The shape of the fetch function callers must supply.
 * It receives the current query string and an AbortSignal.
 */
export type QueryFn<TData> = (
  query: string,
  signal: AbortSignal,
) => Promise<TData>;

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------

export function useDebouncedQuery<TData>(
  query: string,
  queryFn: QueryFn<TData>,
  options: UseDebouncedQueryOptions = {},
): UseDebouncedQueryResult<TData> {
  const { debounceMs = 300, skip = false } = options;

  const [data, setData] = useState<TData | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<Error | null>(null);

  /**
   * Monotonically increasing request id. We store it in a ref so we can compare
   * the id captured at call time with the latest value at resolution time, without
   * triggering extra re-renders.
   */
  const requestIdRef = useRef(0);

  /**
   * Hold the AbortController for the in-flight request so we can cancel it when
   * a new query arrives or when the component unmounts.
   */
  const abortControllerRef = useRef<AbortController | null>(null);

  /**
   * Core fetch logic, extracted so both the debounced effect and refetch() can
   * call it with the same behaviour.
   */
  const execute = useCallback(
    (currentQuery: string) => {
      // Abort any request that is still in flight.
      abortControllerRef.current?.abort();

      const controller = new AbortController();
      abortControllerRef.current = controller;

      // Claim a new request id before going async.
      const id = ++requestIdRef.current;

      setLoading(true);
      setError(null);

      queryFn(currentQuery, controller.signal)
        .then((result) => {
          // Discard the result if a newer request has already been issued.
          if (id !== requestIdRef.current) return;
          setData(result);
        })
        .catch((err: unknown) => {
          if (id !== requestIdRef.current) return;

          // AbortError means we cancelled deliberately — not an application error.
          if (err instanceof Error && err.name === 'AbortError') return;

          setError(err instanceof Error ? err : new Error(String(err)));
        })
        .finally(() => {
          if (id !== requestIdRef.current) return;
          setLoading(false);
        });
    },
    [queryFn],
  );

  // ---------------------------------------------------------------------------
  // Debounced effect — fires whenever query, skip, debounceMs, or execute changes
  // ---------------------------------------------------------------------------

  useEffect(() => {
    if (skip) {
      // Clear stale state when the hook is asked to sit idle.
      abortControllerRef.current?.abort();
      setData(null);
      setLoading(false);
      setError(null);
      return;
    }

    const timer = setTimeout(() => {
      execute(query);
    }, debounceMs);

    return () => {
      clearTimeout(timer);
    };
  }, [query, skip, debounceMs, execute]);

  // ---------------------------------------------------------------------------
  // Cleanup on unmount — abort any pending request
  // ---------------------------------------------------------------------------

  useEffect(() => {
    return () => {
      abortControllerRef.current?.abort();
    };
  }, []);

  // ---------------------------------------------------------------------------
  // refetch — bypasses the debounce and re-runs immediately
  // ---------------------------------------------------------------------------

  const refetch = useCallback(() => {
    if (!skip) {
      execute(query);
    }
  }, [skip, execute, query]);

  return { data, loading, error, refetch };
}
