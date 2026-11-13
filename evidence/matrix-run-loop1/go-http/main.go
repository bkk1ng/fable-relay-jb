// Package main implements a JSON HTTP API server with a middleware chain
// (request ID, structured logging, panic recovery, token auth), two typed
// request/response handlers, and graceful shutdown on SIGTERM/SIGINT.
//
// Go 1.22+, standard library only.
//
// Build & run:
//
//	go mod init example && go build -o server && ./server
//	AUTH_TOKEN=secret ./server
//
// Example requests:
//
//	curl -s -X POST localhost:8080/users \
//	  -H "Authorization: Bearer secret" \
//	  -H "Content-Type: application/json" \
//	  -d '{"name":"Ada","email":"ada@analytical.engine"}'
//
//	curl -s localhost:8080/users/1 -H "Authorization: Bearer secret"
package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// ---------------------------------------------------------------------------
// context key
// ---------------------------------------------------------------------------

type ctxKey string

const requestIDKey ctxKey = "request_id"

// requestIDFromContext returns the request ID stored by requestIDMiddleware,
// or an empty string if none is present.
func requestIDFromContext(ctx context.Context) string {
	v, _ := ctx.Value(requestIDKey).(string
	)
	return v
}

// ---------------------------------------------------------------------------
// request ID middleware
// ---------------------------------------------------------------------------

// generateRequestID returns a 32-character hex string from 16 random bytes.
// If the cryptographic random source is unavailable it falls back to a
// timestamp-based value so the middleware never blocks a request.
func generateRequestID() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		return strconv.FormatInt(time.Now().UnixNano(), 16)
	}
	return hex.EncodeToString(b)
}

// requestIDMiddleware reads or generates an X-Request-ID, writes it to the
// response header, and stores it in the request context for downstream
// middleware and handlers.
func requestIDMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := r.Header.Get("X-Request-ID")
		if id == "" {
			id = generateRequestID()
		}
		w.Header().Set("X-Request-ID", id)
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), requestIDKey, id)))
	})
}

// ---------------------------------------------------------------------------
// status-capturing response writer
// ---------------------------------------------------------------------------

// statusWriter wraps an http.ResponseWriter to capture the status code and
// bytes written so the logging middleware can record them.
type statusWriter struct {
	http.ResponseWriter
	status int
	size   int
}

func (w *statusWriter) WriteHeader(code int) {
	if w.status == 0 {
		w.status = code
	}
	w.ResponseWriter.WriteHeader(code)
}

func (w *statusWriter) Write(b []byte) (int, error) {
	if w.status == 0 {
		w.status = http.StatusOK
	}
	n, err := w.ResponseWriter.Write(b)
	w.size += n
	return n, err
}

// Unwrap allows the standard library to discover optional interfaces like
// http.Flusher and http.Hijacker on the underlying writer (Go 1.20+).
func (w *statusWriter) Unwrap() http.ResponseWriter { return w.ResponseWriter }

// ---------------------------------------------------------------------------
// structured logging middleware
// ---------------------------------------------------------------------------

// loggingMiddleware logs every completed request with slog, including method,
// path, status, response size, duration, remote address, and the request ID
// from the context.
func loggingMiddleware(logger *slog.Logger) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			start := time.Now()
			sw := &statusWriter{ResponseWriter: w}
			next.ServeHTTP(sw, r)
			logger.LogAttrs(r.Context(), slog.LevelInfo, "http request",
				slog.String("method", r.Method),
				slog.String("path", r.URL.Path),
				slog.Int("status", sw.status),
				slog.Int("bytes", sw.size),
				slog.Duration("duration_ms", time.Since(start)),
				slog.String("remote_addr", r.RemoteAddr),
				slog.String("request_id", requestIDFromContext(r.Context())),
			)
		})
	}
}

// ---------------------------------------------------------------------------
// panic recovery middleware
// ---------------------------------------------------------------------------

// recoveryMiddleware recovers from panics in downstream handlers, logs the
// panic value and stack trace, and returns a 500 JSON response that includes
// the request ID so clients can correlate the error in logs.
func recoveryMiddleware(logger *slog.Logger) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			defer func() {
				if rec := recover(); rec != nil {
					logger.LogAttrs(r.Context(), slog.LevelError, "panic recovered",
						slog.Any("panic", rec),
						slog.String("path", r.URL.Path),
						slog.String("request_id", requestIDFromContext(r.Context())),
					)
					writeError(w, http.StatusInternalServerError, "internal server error")
				}
			}()
			next.ServeHTTP(w, r)
		})
	}
}

// ---------------------------------------------------------------------------
// token auth middleware
// ---------------------------------------------------------------------------

// authMiddleware validates a "Bearer <token>" Authorization header against the
// configured token. Requests without a valid token receive a 401.
func authMiddleware(token string) func(http.Handler) http.Handler {
	const prefix = "Bearer "
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			h := r.Header.Get("Authorization")
			if len(h) <= len(prefix) || h[:len(prefix)] != prefix {
				writeError(w, http.StatusUnauthorized, "missing or invalid Authorization header")
				return
			}
			if h[len(prefix):] != token {
				writeError(w, http.StatusUnauthorized, "invalid token")
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

// ---------------------------------------------------------------------------
// typed request / response structs
// ---------------------------------------------------------------------------

type User struct {
	ID    int64  `json:"id"`
	Name  string `json:"name"`
	Email string `json:"email"`
}

type CreateUserRequest struct {
	Name  string `json:"name"`
	Email string `json:"email"`
}

type CreateUserResponse struct {
	User User `json:"user"`
}

type GetUserResponse struct {
	User User `json:"user"`
}

// ---------------------------------------------------------------------------
// in-memory store (demo only)
// ---------------------------------------------------------------------------

var (
	storeMu  sync.RWMutex
	users    = make(map[int64]User)
	userSeq  atomic.Int64
)

// ---------------------------------------------------------------------------
// handlers
// ---------------------------------------------------------------------------

func createUserHandler(logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req CreateUserRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeError(w, http.StatusBadRequest, "invalid JSON body")
			return
		}
		if req.Name == "" {
			writeError(w, http.StatusBadRequest, "name is required")
			return
		}
		if req.Email == "" {
			writeError(w, http.StatusBadRequest, "email is required")
			return
		}

		id := userSeq.Add(1)
		user := User{ID: id, Name: req.Name, Email: req.Email}

		storeMu.Lock()
		users[id] = user
		storeMu.Unlock()

		logger.LogAttrs(r.Context(), slog.LevelInfo, "user created",
			slog.Int64("user_id", id),
			slog.String("name", req.Name),
		)
		writeJSON(w, http.StatusCreated, CreateUserResponse{User: user})
	}
}

func getUserHandler(logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
		if err != nil {
			writeError(w, http.StatusBadRequest, "invalid user id")
			return
		}

		storeMu.RLock()
		user, ok := users[id]
		storeMu.RUnlock()

		if !ok {
			writeError(w, http.StatusNotFound, "user not found")
			return
		}
		writeJSON(w, http.StatusOK, GetUserResponse{User: user})
	}
}

// ---------------------------------------------------------------------------
// middleware chain builder
// ---------------------------------------------------------------------------

// chainMiddleware composes middleware so the first argument runs first.
// Given chainMiddleware(A, B, C) the execution order is A -> B -> C -> handler.
func chainMiddleware(mw ...func(http.Handler) http.Handler) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		for i := len(mw) - 1; i >= 0; i-- {
			next = mw[i](next)
		}
		return next
	}
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	}))
	slog.SetDefault(logger)

	authToken := os.Getenv("AUTH_TOKEN")
	if authToken == "" {
		authToken = "dev-secret-token"
		logger.Info("AUTH_TOKEN not set, using default dev token")
	}

	mux := http.NewServeMux()
	mux.HandleFunc("POST /users", createUserHandler(logger))
	mux.HandleFunc("GET /users/{id}", getUserHandler(logger))

	// Execution order: requestID -> logging -> recovery -> auth -> handler
	chain := chainMiddleware(
		requestIDMiddleware,
		loggingMiddleware(logger),
		recoveryMiddleware(logger),
		authMiddleware(authToken),
	)

	server := &http.Server{
		Addr:         ":8080",
		Handler:      chain(mux),
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 15 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

	// Run the server in a goroutine so the main goroutine can wait for
	// shutdown signals.
	serverErr := make(chan error, 1)
	go func() {
		logger.Info("server starting", slog.String("addr", server.Addr))
		err := server.ListenAndServe()
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			serverErr <- err
		}
	}()

	// Block until SIGTERM or SIGINT arrives, or the server exits on its own.
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	select {
	case <-ctx.Done():
		logger.Info("shutdown signal received")
	case err := <-serverErr:
		logger.Error("server error", slog.String("err", err.Error()))
		return
	}

	// Give in-flight requests up to 30 seconds to complete before forcing
	// the server to stop.
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	if err := server.Shutdown(shutdownCtx); err != nil {
		logger.Error("graceful shutdown failed", slog.String("err", err.Error()))
		return
	}
	logger.Info("server stopped cleanly")
}
