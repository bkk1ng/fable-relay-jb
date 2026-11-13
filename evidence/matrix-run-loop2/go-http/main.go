package main

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

// ---- domain types ----

type CreateUserRequest struct {
	Name  string `json:"name"`
	Email string `json:"email"`
}

type CreateUserResponse struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Email string `json:"email"`
}

type GetStatusResponse struct {
	Status  string `json:"status"`
	Version string `json:"version"`
}

// ---- context keys ----

type contextKey string

const requestIDKey contextKey = "request_id"

// ---- id generation ----

// newID returns a random hex string suitable for use as a request or entity ID.
func newID() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		// crypto/rand failure is a system-level problem; panic is appropriate.
		panic(fmt.Sprintf("crypto/rand unavailable: %v", err))
	}
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:])
}

// ---- middleware ----

// requestID attaches a unique request ID to every request context and response header.
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

			rid, _ := r.Context().Value(requestIDKey).(string)
			logger.Info("request",
				slog.String("request_id", rid),
				slog.String("method", r.Method),
				slog.String("path", r.URL.Path),
				slog.Int("status", rw.status),
				slog.Duration("duration", time.Since(start)),
			)
		})
	}
}

// panicRecovery catches panics, logs them, and writes a 500 response.
func panicRecovery(logger *slog.Logger) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			defer func() {
				if rec := recover(); rec != nil {
					rid, _ := r.Context().Value(requestIDKey).(string)
					logger.Error("panic recovered",
						slog.String("request_id", rid),
						slog.Any("panic", rec),
					)
					http.Error(w, http.StatusText(http.StatusInternalServerError), http.StatusInternalServerError)
				}
			}()
			next.ServeHTTP(w, r)
		})
	}
}

// tokenAuth rejects requests that do not carry the expected bearer token.
// Set token to "" to disable auth (e.g. in tests).
func tokenAuth(token string) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if token == "" {
				next.ServeHTTP(w, r)
				return
			}
			if r.Header.Get("Authorization") != "Bearer "+token {
				http.Error(w, "unauthorized", http.StatusUnauthorized)
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

// responseWriter wraps http.ResponseWriter so we can capture the status code
// after the handler writes it.
type responseWriter struct {
	http.ResponseWriter
	status int
}

func (rw *responseWriter) WriteHeader(code int) {
	rw.status = code
	rw.ResponseWriter.WriteHeader(code)
}

// ---- helpers ----

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(v); err != nil {
		slog.Error("json encode", slog.Any("err", err))
	}
}

func decodeJSON(r *http.Request, v any) error {
	d := json.NewDecoder(r.Body)
	d.DisallowUnknownFields()
	return d.Decode(v)
}

// ---- handlers ----

// handleCreateUser accepts POST /users with a JSON body and returns a
// synthetic user record with a generated ID.
func handleCreateUser(logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		var req CreateUserRequest
		if err := decodeJSON(r, &req); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}

		if req.Name == "" || req.Email == "" {
			writeJSON(w, http.StatusUnprocessableEntity, map[string]string{
				"error": "name and email are required",
			})
			return
		}

		rid, _ := r.Context().Value(requestIDKey).(string)
		logger.Info("creating user",
			slog.String("request_id", rid),
			slog.String("email", req.Email),
		)

		writeJSON(w, http.StatusCreated, CreateUserResponse{
			ID:    newID(),
			Name:  req.Name,
			Email: req.Email,
		})
	}
}

// handleGetStatus returns a simple health/version payload.
func handleGetStatus() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		writeJSON(w, http.StatusOK, GetStatusResponse{
			Status:  "ok",
			Version: "1.0.0",
		})
	}
}

// ---- middleware chain ----

// chain applies middlewares left-to-right, so the first entry is the outermost wrapper.
func chain(h http.Handler, middlewares ...func(http.Handler) http.Handler) http.Handler {
	for i := len(middlewares) - 1; i >= 0; i-- {
		h = middlewares[i](h)
	}
	return h
}

// ---- main ----

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	}))

	// Set API_TOKEN in the environment to enable bearer-token auth.
	// Leave it unset (or empty) to run without auth — useful for local dev.
	token := os.Getenv("API_TOKEN")

	mux := http.NewServeMux()
	mux.Handle("/users", handleCreateUser(logger))
	mux.Handle("/status", handleGetStatus())

	srv := &http.Server{
		Addr: ":8080",
		Handler: chain(mux,
			requestID,
			structuredLogger(logger),
			panicRecovery(logger),
			tokenAuth(token),
		),
		ReadTimeout:  5 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  120 * time.Second,
	}

	go func() {
		logger.Info("server starting", slog.String("addr", srv.Addr))
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("server error", slog.Any("err", err))
			os.Exit(1)
		}
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGTERM, syscall.SIGINT)
	<-quit

	logger.Info("shutting down")
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	if err := srv.Shutdown(ctx); err != nil {
		logger.Error("shutdown error", slog.Any("err", err))
		os.Exit(1)
	}
	logger.Info("server stopped")
}
