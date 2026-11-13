package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/google/uuid"
)

// ── context keys ────────────────────────────────────────────────────────────

type contextKey string

const requestIDKey contextKey = "requestID"

// ── request / response types ─────────────────────────────────────────────────

type CreateItemRequest struct {
	Name        string `json:"name"`
	Description string `json:"description"`
}

type CreateItemResponse struct {
	ID          string    `json:"id"`
	Name        string    `json:"name"`
	Description string    `json:"description"`
	CreatedAt   time.Time `json:"created_at"`
}

type GetStatusRequest struct {
	// parsed from query params, kept as a struct for symmetry
	Verbose bool
}

type GetStatusResponse struct {
	Status    string    `json:"status"`
	Timestamp time.Time `json:"timestamp"`
	RequestID string    `json:"request_id,omitempty"`
	Detail    string    `json:"detail,omitempty"`
}

type ErrorResponse struct {
	Error     string `json:"error"`
	RequestID string `json:"request_id,omitempty"`
}

// ── helpers ──────────────────────────────────────────────────────────────────

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(v); err != nil {
		slog.Error("failed to write response", "error", err)
	}
}

func requestIDFromCtx(ctx context.Context) string {
	if id, ok := ctx.Value(requestIDKey).(string); ok {
		return id
	}
	return ""
}

// ── middleware ───────────────────────────────────────────────────────────────

// requestID attaches a unique ID to every request and sets it on the response.
func requestID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := r.Header.Get("X-Request-ID")
		if id == "" {
			id = newID()
		}
		ctx := context.WithValue(r.Context(), requestIDKey, id)
		w.Header().Set("X-Request-ID", id)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

// structuredLogger logs method, path, status, and duration via slog.
func structuredLogger(logger *slog.Logger) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			start := time.Now()
			rw := &responseWriter{ResponseWriter: w, status: http.StatusOK}

			next.ServeHTTP(rw, r)

			logger.Info("request",
				"method", r.Method,
				"path", r.URL.Path,
				"status", rw.status,
				"duration_ms", time.Since(start).Milliseconds(),
				"request_id", requestIDFromCtx(r.Context()),
			)
		})
	}
}

// panicRecovery catches panics, logs them, and returns 500.
func panicRecovery(logger *slog.Logger) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			defer func() {
				if rec := recover(); rec != nil {
					id := requestIDFromCtx(r.Context())
					logger.Error("panic recovered",
						"panic", fmt.Sprintf("%v", rec),
						"request_id", id,
					)
					writeJSON(w, http.StatusInternalServerError, ErrorResponse{
						Error:     "internal server error",
						RequestID: id,
					})
				}
			}()
			next.ServeHTTP(w, r)
		})
	}
}

// tokenAuth rejects requests whose Authorization header does not match the
// expected bearer token. Pass an empty string to disable auth.
func tokenAuth(token string) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if token == "" {
				next.ServeHTTP(w, r)
				return
			}
			got := r.Header.Get("Authorization")
			if got != "Bearer "+token {
				writeJSON(w, http.StatusUnauthorized, ErrorResponse{
					Error:     "unauthorized",
					RequestID: requestIDFromCtx(r.Context()),
				})
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

// ── response writer wrapper ───────────────────────────────────────────────────

type responseWriter struct {
	http.ResponseWriter
	status int
}

func (rw *responseWriter) WriteHeader(code int) {
	rw.status = code
	rw.ResponseWriter.WriteHeader(code)
}

// ── handlers ─────────────────────────────────────────────────────────────────

// handleCreateItem accepts a JSON body and echoes back a created item.
func handleCreateItem(logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			writeJSON(w, http.StatusMethodNotAllowed, ErrorResponse{
				Error:     "method not allowed",
				RequestID: requestIDFromCtx(r.Context()),
			})
			return
		}

		var req CreateItemRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, http.StatusBadRequest, ErrorResponse{
				Error:     "invalid JSON body",
				RequestID: requestIDFromCtx(r.Context()),
			})
			return
		}
		if req.Name == "" {
			writeJSON(w, http.StatusUnprocessableEntity, ErrorResponse{
				Error:     "name is required",
				RequestID: requestIDFromCtx(r.Context()),
			})
			return
		}

		resp := CreateItemResponse{
			ID:          newID(),
			Name:        req.Name,
			Description: req.Description,
			CreatedAt:   time.Now().UTC(),
		}

		logger.Info("item created", "id", resp.ID, "name", resp.Name,
			"request_id", requestIDFromCtx(r.Context()))

		writeJSON(w, http.StatusCreated, resp)
	}
}

// handleGetStatus returns service health and optionally extra detail.
func handleGetStatus(logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			writeJSON(w, http.StatusMethodNotAllowed, ErrorResponse{
				Error:     "method not allowed",
				RequestID: requestIDFromCtx(r.Context()),
			})
			return
		}

		req := GetStatusRequest{
			Verbose: r.URL.Query().Get("verbose") == "true",
		}

		resp := GetStatusResponse{
			Status:    "ok",
			Timestamp: time.Now().UTC(),
			RequestID: requestIDFromCtx(r.Context()),
		}
		if req.Verbose {
			resp.Detail = "all systems nominal"
		}

		logger.Info("status checked", "verbose", req.Verbose,
			"request_id", requestIDFromCtx(r.Context()))

		writeJSON(w, http.StatusOK, resp)
	}
}

// ── id generation (stdlib only) ───────────────────────────────────────────────

// newID returns a pseudo-random hex string without importing crypto/rand
// in a way that creates friction. Using crypto/rand directly for simplicity.
func newID() string {
	// 16 random bytes → 32 hex chars, good enough for a request / resource ID.
	b := make([]byte, 16)
	if _, err := crand.Read(b); err != nil {
		// fall back to time-based if entropy source fails (extremely unlikely)
		return fmt.Sprintf("%x", time.Now().UnixNano())
	}
	return fmt.Sprintf("%x", b)
}

// ── server ────────────────────────────────────────────────────────────────────

func newServer(addr, authToken string, logger *slog.Logger) *http.Server {
	mux := http.NewServeMux()
	mux.Handle("/items", handleCreateItem(logger))
	mux.Handle("/status", handleGetStatus(logger))

	// Build the middleware chain: request-id → logger → panic-recovery → auth → mux
	var h http.Handler = mux
	h = tokenAuth(authToken)(h)
	h = panicRecovery(logger)(h)
	h = structuredLogger(logger)(h)
	h = requestID(h)

	return &http.Server{
		Addr:         addr,
		Handler:      h,
		ReadTimeout:  5 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  120 * time.Second,
	}
}

// ── main ──────────────────────────────────────────────────────────────────────

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	}))

	addr := envOr("ADDR", ":8080")
	token := envOr("AUTH_TOKEN", "") // empty → auth disabled

	srv := newServer(addr, token, logger)

	// Start in a goroutine so we can listen for shutdown signals.
	idleConnsClosed := make(chan struct{})
	go func() {
		quit := make(chan os.Signal, 1)
		signal.Notify(quit, syscall.SIGTERM, syscall.SIGINT)
		<-quit

		logger.Info("shutdown signal received")

		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()

		if err := srv.Shutdown(ctx); err != nil {
			logger.Error("graceful shutdown failed", "error", err)
		}
		close(idleConnsClosed)
	}()

	logger.Info("server starting", "addr", addr, "auth", token != "")

	if err := srv.ListenAndServe(); !errors.Is(err, http.ErrServerClosed) {
		logger.Error("server error", "error", err)
		os.Exit(1)
	}

	<-idleConnsClosed
	logger.Info("server stopped")
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
