/**
 * useDebouncedQuery
 *
 * A hook that debounces a query string and fetches data from a URL,
 * handling race conditions and in-flight request cancellation.
 *
 * @param query     - The raw query string to debounce (e.g. from an input)
 * @param fetcher   - Async function that accepts (query, signal) and returns data.
 *                    The AbortSignal is passed so the fetcher can cancel the request.
 * @param options   - Optional configuration
 *   @param delay         - Debounce delay in ms (default: 300)
 *   @param minLength     - Minimum query length before fetching (default: 1)
 *   @param enabled       - Set false to pause all fetching (default: true)
 *
 * @returns { data, loading, error, refetch }
 *   - data     Latest successful response, or null
 *   - loading  True while a debounced request is in flight
 *   - error    Last error thrown by the fetcher, or null
 *   - refetch  Imperatively re-run the last query immediately (bypasses debounce)
 *
 * Usage example:
 * ──────────────────────────────────────────────────────────────────────────────
 * interface SearchResult { id: string; title: string }
 *
 * const fetcher = async (q: string, signal: AbortSignal): Promise<SearchResult[]> => {
 *   const res = await fetch(`/api/search?q=${encodeURIComponent(q)}`, { signal })
 *   if (!res.ok) throw new Error(`HTTP ${res.status}`)
 *   return res.json()
 * }
 *
 * function SearchBox() {
 *   const [input, setInput] = useState('')
 *   const { data, loading, error, refetch } = useDebouncedQuery(input, fetcher, {
 *     delay: 400,
 *     minLength: 2,
 *   })
 *
 *   return (
 *     <>
 *       <input value={input} onChange={e => setInput(e.target.value)} />
 *       {loading && <span>Loading…</span>}
 *       {error && <span>Error: {error.message}</span>}
 *       {data?.map(r => <div key={r.id}>{r.title}</div>)}
 *       <button onClick={refetch}>Refresh</button>
 *     </>
 *   )
 * }
 * ──────────────────────────────────────────────────────────────────────────────
 */

import { useState, useEffect, useCallback, useRef } from 'react'

export interface UseDebouncedQueryOptions {
  /** Debounce delay in milliseconds. Default: 300 */
  delay?: number
  /** Minimum query length required to trigger a fetch. Default: 1 */
  minLength?: number
  /** Set to false to disable fetching entirely. Default: true */
  enabled?: boolean
}

export interface UseDebouncedQueryResult<T> {
  data: T | null
  loading: boolean
  error: Error | null
  refetch: () => void
}

export type QueryFetcher<T> = (query: string, signal: AbortSignal) => Promise<T>

export function useDebouncedQuery<T>(
  query: string,
  fetcher: QueryFetcher<T>,
  options: UseDebouncedQueryOptions = {},
): UseDebouncedQueryResult<T> {
  const { delay = 300, minLength = 1, enabled = true } = options

  const [data, setData] = useState<T | null>(null)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<Error | null>(null)

  // Tracks the latest request id so stale responses are discarded.
  const requestIdRef = useRef(0)
  // Holds the AbortController for the in-flight request so we can cancel it.
  const controllerRef = useRef<AbortController | null>(null)
  // Keeps a stable reference to the latest fetcher without resetting the effect.
  const fetcherRef = useRef(fetcher)
  useEffect(() => { fetcherRef.current = fetcher }, [fetcher])

  const execute = useCallback((q: string) => {
    // Cancel any in-flight request before starting a new one.
    controllerRef.current?.abort()

    if (!enabled || q.length < minLength) {
      setLoading(false)
      return
    }

    const controller = new AbortController()
    controllerRef.current = controller

    // Increment and capture the id for this request.
    const id = ++requestIdRef.current

    setLoading(true)
    setError(null)

    fetcherRef.current(q, controller.signal)
      .then(result => {
        // Discard the response if a newer request has already been issued.
        if (id !== requestIdRef.current) return
        setData(result)
        setError(null)
      })
      .catch(err => {
        if (id !== requestIdRef.current) return
        // Ignore intentional cancellations — they're not real errors.
        if (err instanceof DOMException && err.name === 'AbortError') return
        setError(err instanceof Error ? err : new Error(String(err)))
      })
      .finally(() => {
        if (id !== requestIdRef.current) return
        setLoading(false)
      })
  }, [enabled, minLength])

  // Debounce: wait for the query to settle before firing.
  useEffect(() => {
    const timer = setTimeout(() => execute(query), delay)
    return () => clearTimeout(timer)
  }, [query, delay, execute])

  // Cleanup: abort the in-flight request when the component unmounts.
  useEffect(() => {
    return () => { controllerRef.current?.abort() }
  }, [])

  // Exposes an imperative escape hatch that bypasses the debounce.
  const refetch = useCallback(() => execute(query), [execute, query])

  return { data, loading, error, refetch }
}
