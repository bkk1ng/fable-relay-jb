import http from "node:http";
import { IncomingMessage, ServerResponse } from "node:http";

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

export type HttpMethod = "GET" | "POST" | "PUT" | "PATCH" | "DELETE" | "HEAD" | "OPTIONS";

export interface RouteParams {
  [key: string]: string;
}

export interface AppRequest extends IncomingMessage {
  params: RouteParams;
  query: Record<string, string>;
  body: unknown;
}

export interface AppResponse extends ServerResponse {
  json: (statusCode: number, data: unknown) => void;
}

export type Handler = (req: AppRequest, res: AppResponse) => void | Promise<void>;

export type ErrorHandler = (
  err: unknown,
  req: AppRequest,
  res: AppResponse
) => void | Promise<void>;

interface Route {
  method: HttpMethod;
  pattern: RegExp;
  paramNames: string[];
  handler: Handler;
}

// ---------------------------------------------------------------------------
// Schema validation helpers
// ---------------------------------------------------------------------------

export type ValidationResult<T> =
  | { ok: true; value: T }
  | { ok: false; errors: string[] };

export type Schema<T> = { validate: (input: unknown) => ValidationResult<T> };

/** Build a schema that checks required string fields and optional string fields. */
export function objectSchema<T extends Record<string, unknown>>(opts: {
  required?: (keyof T)[];
  optional?: (keyof T)[];
  extraAllowed?: boolean;
}): Schema<T> {
  return {
    validate(input: unknown): ValidationResult<T> {
      if (typeof input !== "object" || input === null || Array.isArray(input)) {
        return { ok: false, errors: ["Expected an object"] };
      }
      const obj = input as Record<string, unknown>;
      const errors: string[] = [];
      for (const key of opts.required ?? []) {
        if (!(key as string in obj) || obj[key as string] === undefined) {
          errors.push(`Missing required field: ${String(key)}`);
        }
      }
      if (!opts.extraAllowed) {
        const allowed = new Set([
          ...(opts.required ?? []).map(String),
          ...(opts.optional ?? []).map(String),
        ]);
        for (const k of Object.keys(obj)) {
          if (!allowed.has(k)) errors.push(`Unknown field: ${k}`);
        }
      }
      return errors.length ? { ok: false, errors } : { ok: true, value: input as T };
    },
  };
}

export function validateBody<T>(
  body: unknown,
  schema: Schema<T>
): ValidationResult<T> {
  return schema.validate(body);
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

function compilePath(path: string): { regex: RegExp; paramNames: string[] } {
  const paramNames: string[] = [];
  const escaped = path
    .replace(/[-[\]{}()*+?.,\\^$|#\s]/g, (c) =>
      c === "/" ? "/" : `\\${c}`
    )
    .replace(/:([A-Za-z_][A-Za-z0-9_]*)/g, (_match, name: string) => {
      paramNames.push(name);
      return "([^/]+)";
    });
  return { regex: new RegExp(`^${escaped}$`), paramNames };
}

function parseQuery(url: string): Record<string, string> {
  const idx = url.indexOf("?");
  if (idx === -1) return {};
  const result: Record<string, string> = {};
  for (const [k, v] of new URLSearchParams(url.slice(idx + 1))) {
    result[k] = v;
  }
  return result;
}

async function readBody(req: IncomingMessage, limitBytes: number): Promise<string> {
  return new Promise((resolve, reject) => {
    let data = "";
    let size = 0;
    req.setEncoding("utf8");
    req.on("data", (chunk: string) => {
      size += Buffer.byteLength(chunk, "utf8");
      if (size > limitBytes) {
        req.destroy();
        reject(new PayloadTooLargeError(limitBytes));
        return;
      }
      data += chunk;
    });
    req.on("end", () => resolve(data));
    req.on("error", reject);
  });
}

// ---------------------------------------------------------------------------
// Custom errors
// ---------------------------------------------------------------------------

export class HttpError extends Error {
  constructor(
    public readonly statusCode: number,
    message: string,
    public readonly details?: unknown
  ) {
    super(message);
    this.name = "HttpError";
  }
}

export class PayloadTooLargeError extends HttpError {
  constructor(limitBytes: number) {
    super(413, `Payload exceeds limit of ${limitBytes} bytes`);
    this.name = "PayloadTooLargeError";
  }
}

export class NotFoundError extends HttpError {
  constructor(path: string) {
    super(404, `Route not found: ${path}`);
    this.name = "NotFoundError";
  }
}

export class ValidationError extends HttpError {
  constructor(errors: string[]) {
    super(422, "Validation failed", errors);
    this.name = "ValidationError";
  }
}

// ---------------------------------------------------------------------------
// App (mini framework)
// ---------------------------------------------------------------------------

export interface AppOptions {
  bodyLimitBytes?: number;
}

const DEFAULT_BODY_LIMIT = 1_048_576; // 1 MiB

export function createApp(opts: AppOptions = {}) {
  const bodyLimit = opts.bodyLimitBytes ?? DEFAULT_BODY_LIMIT;
  const routes: Route[] = [];
  let errorHandler: ErrorHandler = defaultErrorHandler;

  function addRoute(method: HttpMethod, path: string, handler: Handler): void {
    const { regex, paramNames } = compilePath(path);
    routes.push({ method, pattern: regex, paramNames, handler });
  }

  const app = {
    get(path: string, handler: Handler) { addRoute("GET", path, handler); return app; },
    post(path: string, handler: Handler) { addRoute("POST", path, handler); return app; },
    put(path: string, handler: Handler) { addRoute("PUT", path, handler); return app; },
    patch(path: string, handler: Handler) { addRoute("PATCH", path, handler); return app; },
    delete(path: string, handler: Handler) { addRoute("DELETE", path, handler); return app; },

    onError(handler: ErrorHandler) {
      errorHandler = handler;
      return app;
    },

    async handle(rawReq: IncomingMessage, rawRes: ServerResponse): Promise<void> {
      const req = rawReq as AppRequest;
      const res = rawRes as AppResponse;

      // Attach json helper
      res.json = (statusCode: number, data: unknown) => {
        const body = JSON.stringify(data);
        res.writeHead(statusCode, {
          "Content-Type": "application/json; charset=utf-8",
          "Content-Length": Buffer.byteLength(body),
        });
        res.end(body);
      };

      const rawUrl = req.url ?? "/";
      const pathname = rawUrl.split("?")[0];
      req.query = parseQuery(rawUrl);
      req.params = {};
      req.body = undefined;

      // Parse body for methods that carry one
      const method = (req.method ?? "GET").toUpperCase() as HttpMethod;
      if (["POST", "PUT", "PATCH"].includes(method)) {
        try {
          const raw = await readBody(req, bodyLimit);
          if (raw.length > 0) {
            const ct = req.headers["content-type"] ?? "";
            if (ct.includes("application/json")) {
              req.body = JSON.parse(raw);
            } else {
              req.body = raw;
            }
          }
        } catch (err) {
          await errorHandler(err, req, res);
          return;
        }
      }

      // Match route
      for (const route of routes) {
        if (route.method !== method) continue;
        const match = pathname.match(route.pattern);
        if (!match) continue;

        // Extract named params
        for (let i = 0; i < route.paramNames.length; i++) {
          req.params[route.paramNames[i]] = decodeURIComponent(match[i + 1]);
        }

        try {
          await route.handler(req, res);
        } catch (err) {
          await errorHandler(err, req, res);
        }
        return;
      }

      await errorHandler(new NotFoundError(pathname), req, res);
    },

    listen(port: number, hostname = "127.0.0.1"): http.Server {
      const server = http.createServer((req, res) => {
        app.handle(req, res).catch((err) => {
          console.error("Unhandled error in request pipeline:", err);
          if (!res.headersSent) res.writeHead(500).end();
        });
      });

      server.listen(port, hostname, () => {
        console.log(`Listening on http://${hostname}:${port}`);
      });

      return server;
    },
  };

  return app;
}

// ---------------------------------------------------------------------------
// Default error handler
// ---------------------------------------------------------------------------

function defaultErrorHandler(err: unknown, _req: AppRequest, res: AppResponse): void {
  if (err instanceof HttpError) {
    const body: Record<string, unknown> = { error: err.message };
    if (err.details !== undefined) body.details = err.details;
    res.json(err.statusCode, body);
    return;
  }

  if (err instanceof SyntaxError) {
    res.json(400, { error: "Invalid JSON" });
    return;
  }

  console.error("Internal error:", err);
  res.json(500, { error: "Internal server error" });
}

// ---------------------------------------------------------------------------
// In-memory todo store
// ---------------------------------------------------------------------------

export interface Todo {
  id: string;
  title: string;
  completed: boolean;
  createdAt: string;
  updatedAt: string;
}

const store = new Map<string, Todo>();
let idCounter = 1;

function generateId(): string {
  return String(idCounter++);
}

// Schemas
const createTodoSchema = objectSchema<{ title: string; completed?: boolean }>({
  required: ["title"],
  optional: ["completed"],
});

const updateTodoSchema = objectSchema<{ title?: string; completed?: boolean }>({
  optional: ["title", "completed"],
});

// ---------------------------------------------------------------------------
// Todo route handlers
// ---------------------------------------------------------------------------

function listTodos(_req: AppRequest, res: AppResponse): void {
  res.json(200, Array.from(store.values()));
}

function getTodo(req: AppRequest, res: AppResponse): void {
  const todo = store.get(req.params.id);
  if (!todo) throw new HttpError(404, `Todo ${req.params.id} not found`);
  res.json(200, todo);
}

function createTodo(req: AppRequest, res: AppResponse): void {
  const result = validateBody(req.body, createTodoSchema);
  if (!result.ok) throw new ValidationError(result.errors);

  const { title, completed = false } = result.value;
  if (typeof title !== "string" || title.trim().length === 0) {
    throw new ValidationError(["title must be a non-empty string"]);
  }

  const now = new Date().toISOString();
  const todo: Todo = {
    id: generateId(),
    title: title.trim(),
    completed,
    createdAt: now,
    updatedAt: now,
  };
  store.set(todo.id, todo);
  res.json(201, todo);
}

function updateTodo(req: AppRequest, res: AppResponse): void {
  const existing = store.get(req.params.id);
  if (!existing) throw new HttpError(404, `Todo ${req.params.id} not found`);

  const result = validateBody(req.body, updateTodoSchema);
  if (!result.ok) throw new ValidationError(result.errors);

  const patch = result.value;
  if (
    patch.title !== undefined &&
    (typeof patch.title !== "string" || patch.title.trim().length === 0)
  ) {
    throw new ValidationError(["title must be a non-empty string"]);
  }

  const updated: Todo = {
    ...existing,
    ...(patch.title !== undefined ? { title: patch.title.trim() } : {}),
    ...(patch.completed !== undefined ? { completed: patch.completed } : {}),
    updatedAt: new Date().toISOString(),
  };
  store.set(updated.id, updated);
  res.json(200, updated);
}

function deleteTodo(req: AppRequest, res: AppResponse): void {
  if (!store.has(req.params.id)) {
    throw new HttpError(404, `Todo ${req.params.id} not found`);
  }
  store.delete(req.params.id);
  res.writeHead(204).end();
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

const PORT = Number(process.env.PORT ?? 3000);
const HOST = process.env.HOST ?? "127.0.0.1";

const app = createApp({ bodyLimitBytes: 65_536 }); // 64 KiB limit

app
  .get("/todos", listTodos)
  .get("/todos/:id", getTodo)
  .post("/todos", createTodo)
  .patch("/todos/:id", updateTodo)
  .delete("/todos/:id", deleteTodo);

const server = app.listen(PORT, HOST);

// ---------------------------------------------------------------------------
// Graceful shutdown
// ---------------------------------------------------------------------------

let shuttingDown = false;

function shutdown(signal: string): void {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log(`\nReceived ${signal}. Shutting down gracefully…`);

  server.close((err) => {
    if (err) {
      console.error("Error during shutdown:", err);
      process.exit(1);
    }
    console.log("Server closed.");
    process.exit(0);
  });

  // Force exit after 10 s if connections linger
  setTimeout(() => {
    console.error("Forced exit after timeout.");
    process.exit(1);
  }, 10_000).unref();
}

process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT", () => shutdown("SIGINT"));
