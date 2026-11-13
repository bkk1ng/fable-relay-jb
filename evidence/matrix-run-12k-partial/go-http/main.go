package main

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"math/rand"
	"strconv"
)

// ---- request/response types ------------------------------------------------

type CreateItemRequest struct {
	Name  string `json:"name"`
	Price int    `json:"price"` // cents
}

type CreateItemResponse struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Price int    `json:"price"`
}

type GetItemResponse struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Price int    `json:"price"`
}

type ErrorResponse struct {
	Error string `json:"error"`
}

// ---- helpers ----------------------------------------------------------------

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(v); err != nil {
		slog.Error("failed to encode response", "err", err)
	}
}

func decodeJSON(r *http.Request, dst any) error {
	r.Body = http.MaxBytesReader(nil, r.Body, 1<<20) // 1 MB
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	return dec.Decode(dst)
}

func randomID() string {
	return strconv.FormatUint(rand.Uint64(), 36)
}

// ---- middleware -------------------------------------------------------------

// requestID attaches a unique request ID to every request and response.
func requestID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := r.Header.Get("X-Request-ID")
		if id == "" {
			id = randomID()
		}
		ctx := context.WithValue(r.Context(), ctxKeyRequestID{}, id)
		w.Header().Set("X-Request-ID", id)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

type ctxKeyRequestID struct{}

func getRequestID(ctx context.Context) string {
	id, _ := ctx.Value(ctxKeyRequestID{}).(string)
	return id
}

// structuredLogger logs method, path, status, and duration via slog.
func structuredLogger(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rw := &responseWriter{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(rw, r)
		slog.Info("request",
			"id", getRequestID(r.Context()),
			"method", r.Method,
			"path", r.URL.Path,
			"status", rw.status,
			"duration_ms", time.Since(start).Milliseconds(),
		)
	})
}

// responseWriter wraps http.ResponseWriter to capture the status code.
type responseWriter struct {
	http.ResponseWriter
	status int
}

func (rw *responseWriter) WriteHeader(code int) {
	rw.status = code
	rw.ResponseWriter.WriteHeader(code)
}

// panicRecovery catches any panic in a handler and returns 500.
func panicRecovery(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if rec := recover(); rec != nil {
				slog.Error("panic recovered",
					"id", getRequestID(r.Context()),
					"panic", rec,
				)
				writeJSON(w, http.StatusInternalServerError, ErrorResponse{Error: "internal server error"})
			}
		}()
		next.ServeHTTP(w, r)
	})
}

// tokenAuth rejects requests whose Authorization header does not match the
// configured bearer token. Pass an empty token to disable auth.
func tokenAuth(token string) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if token == "" {
				next.ServeHTTP(w, r)
				return
			}
			got := r.Header.Get("Authorization")
			if got != "Bearer "+token {
				writeJSON(w, http.StatusUnauthorized, ErrorResponse{Error: "unauthorized"})
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

// chain applies middleware right-to-left so the first argument is outermost.
func chain(h http.Handler, middleware ...func(http.Handler) http.Handler) http.Handler {
	for i := len(middleware) - 1; i >= 0; i-- {
		h = middleware[i](h)
	}
	return h
}

// ---- handlers ---------------------------------------------------------------

// POST /items
// Body: {"name": "widget", "price": 999}
func createItem(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSON(w, http.StatusMethodNotAllowed, ErrorResponse{Error: "method not allowed"})
		return
	}

	var req CreateItemRequest
	if err := decodeJSON(r, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, ErrorResponse{Error: "invalid request body: " + err.Error()})
		return
	}
	if req.Name == "" {
		writeJSON(w, http.StatusUnprocessableEntity, ErrorResponse{Error: "name is required"})
		return
	}
	if req.Price < 0 {
		writeJSON(w, http.StatusUnprocessableEntity, ErrorResponse{Error: "price must be non-negative"})
		return
	}

	resp := CreateItemResponse{
		ID:    randomID(),
		Name:  req.Name,
		Price: req.Price,
	}
	writeJSON(w, http.StatusCreated, resp)
}

// GET /items/{id}
func getItem(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSON(w, http.StatusMethodNotAllowed, ErrorResponse{Error: "method not allowed"})
		return
	}

	// Go 1.22 path value extraction.
	id := r.PathValue("id")
	if id == "" {
		writeJSON(w, http.StatusBadRequest, ErrorResponse{Error: "missing id"})
		return
	}

	// Stub: in a real service this would hit a store.
	resp := GetItemResponse{
		ID:    id,
		Name:  "example item",
		Price: 1000,
	}
	writeJSON(w, http.StatusOK, resp)
}

// ---- server setup -----------------------------------------------------------

func newRouter(authToken string) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("POST /items", createItem)
	mux.HandleFunc("GET /items/{id}", getItem)

	return chain(
		mux,
		requestID,
		structuredLogger,
		panicRecovery,
		tokenAuth(authToken),
	)
}

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, nil))
	slog.SetDefault(logger)

	authToken := os.Getenv("API_TOKEN") // empty → auth disabled

	srv := &http.Server{
		Addr:         ":8080",
		Handler:      newRouter(authToken),
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

	// Start in a goroutine so we can listen for shutdown signals.
	go func() {
		slog.Info("server starting", "addr", srv.Addr)
		if err := srv.ListenAndServe(); !errors.Is(err, http.ErrServerClosed) {
			slog.Error("server error", "err", err)
			os.Exit(1)
		}
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGTERM, syscall.SIGINT)
	<-quit

	slog.Info("shutting down")

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	if err := srv.Shutdown(ctx); err != nil {
		slog.Error("shutdown error", "err", err)
		os.Exit(1)
	}

	slog.Info("stopped")
}
