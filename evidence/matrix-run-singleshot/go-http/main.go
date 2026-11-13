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

	"crypto/rand"
	"encoding/hex"
)

// ── request / response types ──────────────────────────────────────────────────

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

type GetItemRequest struct {
	ID string // populated from path value, not JSON body
}

type GetItemResponse struct {
	ID          string    `json:"id"`
	Name        string    `json:"name"`
	Description string    `json:"description"`
	CreatedAt   time.Time `json:"created_at"`
}

type ErrorResponse struct {
	Error     string `json:"error"`
	RequestID string `json:"request_id,omitempty"`
}

// ── in-memory store (stand-in for a real DB) ─────────────────────────────────

type Item struct {
	ID          string
	Name        string
	Description string
	CreatedAt   time.Time
}

type Store struct {
	items map[string]Item
}

func NewStore() *Store {
	return &Store{items: make(map[string]Item)}
}

func (s *Store) Create(name, description string) Item {
	item := Item{
		ID:          newID(),
		Name:        name,
		Description: description,
		CreatedAt:   time.Now().UTC(),
	}
	s.items[item.ID] = item
	return item
}

func (s *Store) Get(id string) (Item, bool) {
	item, ok := s.items[id]
	return item, ok
}

// ── context keys ──────────────────────────────────────────────────────────────

type contextKey string

const requestIDKey contextKey = "request_id"

func requestIDFromCtx(ctx context.Context) string {
	if v, ok := ctx.Value(requestIDKey).(string); ok {
		return v
	}
	return ""
}

// ── middleware ────────────────────────────────────────────────────────────────

// chain applies middleware in left-to-right order (first = outermost).
func chain(h http.Handler, middleware ...func(http.Handler) http.Handler) http.Handler {
	for i := len(middleware) - 1; i >= 0; i-- {
		h = middleware[i](h)
	}
	return h
}

// requestID attaches a unique request ID to every request context and response.
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

// structuredLogger logs method, path, status, and latency using slog.
func structuredLogger(logger *slog.Logger) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			start := time.Now()
			rw := &responseWriter{ResponseWriter: w, status: http.StatusOK}

			next.ServeHTTP(rw, r)

			logger.InfoContext(r.Context(), "request",
				slog.String("method", r.Method),
				slog.String("path", r.URL.Path),
				slog.Int("status", rw.status),
				slog.Duration("latency", time.Since(start)),
				slog.String("request_id", requestIDFromCtx(r.Context())),
			)
		})
	}
}

// panicRecovery catches any panic, logs it, and returns 500.
func panicRecovery(logger *slog.Logger) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			defer func() {
				if rec := recover(); rec != nil {
					logger.ErrorContext(r.Context(), "panic recovered",
						slog.Any("error", rec),
						slog.String("request_id", requestIDFromCtx(r.Context())),
					)
					writeJSON(w, http.StatusInternalServerError, ErrorResponse{
						Error:     "internal server error",
						RequestID: requestIDFromCtx(r.Context()),
					})
				}
			}()
			next.ServeHTTP(w, r)
		})
	}
}

// tokenAuth rejects requests whose Authorization header does not match the
// expected "Bearer <token>" value.
func tokenAuth(token string) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			expected := "Bearer " + token
			if r.Header.Get("Authorization") != expected {
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

// ── handlers ─────────────────────────────────────────────────────────────────

type server struct {
	store  *Store
	logger *slog.Logger
}

// POST /items
func (s *server) createItem(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSON(w, http.StatusMethodNotAllowed, ErrorResponse{Error: "method not allowed"})
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

	item := s.store.Create(req.Name, req.Description)

	writeJSON(w, http.StatusCreated, CreateItemResponse{
		ID:          item.ID,
		Name:        item.Name,
		Description: item.Description,
		CreatedAt:   item.CreatedAt,
	})
}

// GET /items/{id}
func (s *server) getItem(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSON(w, http.StatusMethodNotAllowed, ErrorResponse{Error: "method not allowed"})
		return
	}

	req := GetItemRequest{ID: r.PathValue("id")}
	if req.ID == "" {
		writeJSON(w, http.StatusBadRequest, ErrorResponse{
			Error:     "id is required",
			RequestID: requestIDFromCtx(r.Context()),
		})
		return
	}

	item, ok := s.store.Get(req.ID)
	if !ok {
		writeJSON(w, http.StatusNotFound, ErrorResponse{
			Error:     fmt.Sprintf("item %q not found", req.ID),
			RequestID: requestIDFromCtx(r.Context()),
		})
		return
	}

	writeJSON(w, http.StatusOK, GetItemResponse{
		ID:          item.ID,
		Name:        item.Name,
		Description: item.Description,
		CreatedAt:   item.CreatedAt,
	})
}

// ── helpers ───────────────────────────────────────────────────────────────────

// responseWriter wraps http.ResponseWriter to capture the status code.
type responseWriter struct {
	http.ResponseWriter
	status int
}

func (rw *responseWriter) WriteHeader(code int) {
	rw.status = code
	rw.ResponseWriter.WriteHeader(code)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(v); err != nil {
		// At this point headers are already sent; best we can do is log.
		slog.Error("writeJSON encode error", slog.Any("error", err))
	}
}

func newID() string {
	b := make([]byte, 8)
	if _, err := rand.Read(b); err != nil {
		panic("failed to generate random ID: " + err.Error())
	}
	return hex.EncodeToString(b)
}

// ── main ──────────────────────────────────────────────────────────────────────

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	}))

	// Read the auth token from the environment; fall back to a dev default.
	authToken := os.Getenv("API_TOKEN")
	if authToken == "" {
		authToken = "dev-token"
		logger.Warn("API_TOKEN not set, using insecure default")
	}

	srv := &server{
		store:  NewStore(),
		logger: logger,
	}

	mux := http.NewServeMux()

	// Apply middleware: request ID → logger → panic recovery → auth → handler.
	middleware := []func(http.Handler) http.Handler{
		requestID,
		structuredLogger(logger),
		panicRecovery(logger),
		tokenAuth(authToken),
	}

	mux.Handle("POST /items", chain(http.HandlerFunc(srv.createItem), middleware...))
	mux.Handle("GET /items/{id}", chain(http.HandlerFunc(srv.getItem), middleware...))

	httpServer := &http.Server{
		Addr:         ":8080",
		Handler:      mux,
		ReadTimeout:  5 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  120 * time.Second,
	}

	// Start server in a goroutine so we can listen for shutdown signals.
	serverErr := make(chan error, 1)
	go func() {
		logger.Info("server starting", slog.String("addr", httpServer.Addr))
		if err := httpServer.ListenAndServe(); !errors.Is(err, http.ErrServerClosed) {
			serverErr <- err
		}
	}()

	// Block until SIGTERM/SIGINT or a startup error.
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGTERM, syscall.SIGINT)

	select {
	case err := <-serverErr:
		logger.Error("server error", slog.Any("error", err))
		os.Exit(1)
	case sig := <-quit:
		logger.Info("shutdown signal received", slog.String("signal", sig.String()))
	}

	// Give in-flight requests up to 10 seconds to finish.
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	if err := httpServer.Shutdown(ctx); err != nil {
		logger.Error("graceful shutdown failed", slog.Any("error", err))
		os.Exit(1)
	}

	logger.Info("server stopped cleanly")
}
