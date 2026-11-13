/**
 * useDebouncedQuery
 *
 * A hook that debounces a query string, fetches data from a URL, and
 * handles race conditions, in-flight cancellation, and cleanup on unmount.
 *
 * Usage:
 *
 *   const { data, loading, error, refetch } = useDebouncedQuery<User[]>(
 *     '/api/users?q=' + query,   // url — recomputed whenever query changes
 *     query,                     // the raw query string to debounce
 *     { debounceMs: 300 }        // optional config
 *   );
 *
 * - `data`    — the parsed JSON response body, or null before the first
 *               successful response.
 * - `loading` — true while a debounced request is in flight.
 * - `error`   — the last Error thrown, or null on success / before first fetch.
 * - `refetch` — imperatively re-run the latest request (bypasses debounce).
 *
 * Notes:
 * - Each new query value cancels the previous in-flight request via
 *   AbortController before issuing a new one.
 * - A monotonically incrementing requestId guards against stale responses:
 *   results from an earlier request that resolves late are silently dropped.
 * - The debounce timer and any active AbortController are cleaned up when
 *   the component unmounts.
 * - Passing an empty string (or any falsy url) skips the fetch entirely.
 */

import { useState, useEffect, useCallback, useRef } from 'react';

export interface UseDebouncedQueryOptions {
  /** Milliseconds to wait after the last query change before fetching. Default: 300. */
  debounceMs?: number;
  /** Additional RequestInit options forwarded to fetch (headers, method, etc.). */
  fetchOptions?: Omit<RequestInit, 'signal'>;
}

export interface UseDebouncedQueryResult<T> {
  data: T | null;
  loading: boolean;
  error: Error | null;
  refetch: () => void;
}

function useDebouncedQuery<T = unknown>(
  url: string,
  query: string,
  options: UseDebouncedQueryOptions = {},
): UseDebouncedQueryResult<T> {
  const { debounceMs = 300, fetchOptions } = options;

  const [data, setData] = useState<T | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<Error | null>(null);

  // Tracks the latest request id so stale responses are discarded.
  const requestIdRef = useRef(0);

  // Holds the AbortController for the currently in-flight request.
  const abortControllerRef = useRef<AbortController | null>(null);

  // Stable ref to fetchOptions so the execute callback doesn't need it as a dep.
  const fetchOptionsRef = useRef(fetchOptions);
  useEffect(() => {
    fetchOptionsRef.current = fetchOptions;
  }, [fetchOptions]);

  /**
   * execute — cancel any in-flight request, then issue a new one.
   * Accepts the request id captured at call-time so it can detect staleness.
   */
  const execute = useCallback(
    (currentUrl: string, currentRequestId: number) => {
      // Cancel the previous in-flight request if one exists.
      if (abortControllerRef.current) {
        abortControllerRef.current.abort();
      }

      if (!currentUrl) {
        setLoading(false);
        return;
      }

      const controller = new AbortController();
      abortControllerRef.current = controller;

      setLoading(true);
      setError(null);

      fetch(currentUrl, {
        ...fetchOptionsRef.current,
        signal: controller.signal,
      })
        .then((response) => {
          if (!response.ok) {
            throw new Error(
              `Request failed: ${response.status} ${response.statusText}`,
            );
          }
          return response.json() as Promise<T>;
        })
        .then((json) => {
          // Drop results from an earlier request that resolved late.
          if (currentRequestId !== requestIdRef.current) return;
          setData(json);
          setLoading(false);
        })
        .catch((err: unknown) => {
          // AbortError is expected when we cancel — don't surface it as an error.
          if (err instanceof DOMException && err.name === 'AbortError') return;
          if (currentRequestId !== requestIdRef.current) return;
          setError(err instanceof Error ? err : new Error(String(err)));
          setLoading(false);
        });
    },
    [],
  );

  // Debounced effect: re-runs whenever url or query changes.
  useEffect(() => {
    const id = ++requestIdRef.current;

    if (!query) {
      // Clear state immediately for an empty query.
      if (abortControllerRef.current) {
        abortControllerRef.current.abort();
      }
      setData(null);
      setLoading(false);
      setError(null);
      return;
    }

    const timer = setTimeout(() => {
      execute(url, id);
    }, debounceMs);

    return () => {
      clearTimeout(timer);
    };
  }, [url, query, debounceMs, execute]);

  // Cleanup: abort any in-flight request when the component unmounts.
  useEffect(() => {
    return () => {
      if (abortControllerRef.current) {
        abortControllerRef.current.abort();
      }
    };
  }, []);

  /**
   * refetch — bypasses the debounce and immediately re-issues the request.
   * Useful for explicit retry after an error.
   */
  const refetch = useCallback(() => {
    const id = ++requestIdRef.current;
    execute(url, id);
  }, [url, execute]);

  return { data, loading, error, refetch };
}

export default useDebouncedQuery;
