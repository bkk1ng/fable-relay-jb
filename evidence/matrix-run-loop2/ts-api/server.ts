import http from "node:http";
import { IncomingMessage, ServerResponse } from "node:http";

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

export type HttpMethod = "GET" | "POST" | "PUT" | "PATCH" | "DELETE";

export interface RouteParams {
  [key: string]: string;
}

export interface ParsedRequest {
  params: RouteParams;
  query: URLSearchParams;
  body: unknown;
}

export type Handler = (
  req: IncomingMessage & ParsedRequest,
  res: TypedResponse
) => void | Promise<void>;

export type ErrorHandler = (
  err: unknown,
  req: IncomingMessage & ParsedRequest,
  res: TypedResponse
) => void | Promise<void>;

export interface TypedResponse extends ServerResponse {
  json: (data: unknown, status?: number) => void;
}

export interface Route {
  method: HttpMethod;
  pattern: RegExp;
  paramNames: string[];
  handler: Handler;
}

export interface ValidationError {
  field: string;
  message: string;
}

export interface ValidationResult<T> {
  ok: boolean;
  value?: T;
  errors: ValidationError[];
}

// ---------------------------------------------------------------------------
// Schema validation helpers
// ---------------------------------------------------------------------------

type Validator<T> = (input: unknown) => ValidationResult<T>;

function string(options?: { minLength?: number; maxLength?: number }): Validator<string> {
  return (input) => {
    if (typeof input !== "string") {
      return { ok: false, errors: [{ field: "", message: "must be a string" }] };
    }
    if (options?.minLength !== undefined && input.length < options.minLength) {
      return {
        ok: false,
        errors: [{ field: "", message: `must be at least ${options.minLength} characters` }],
      };
    }
    if (options?.maxLength !== undefined && input.length > options.maxLength) {
      return {
        ok: false,
        errors: [{ field: "", message: `must be at most ${options.maxLength} characters` }],
      };
    }
    return { ok: true, value: input, errors: [] };
  };
}

function boolean(): Validator<boolean> {
  return (input) => {
    if (typeof input !== "boolean") {
      return { ok: false, errors: [{ field: "", message: "must be a boolean" }] };
    }
    return { ok: true, value: input, errors: [] };
  };
}

type SchemaShape = Record<string, Validator<unknown>>;
type InferSchema<S extends SchemaShape> = {
  [K in keyof S]: S[K] extends Validator<infer T> ? T : never;
};

function object<S extends SchemaShape>(
  shape: S,
  options?: { allowExtra?: boolean }
): Validator<InferSchema<S>> {
  return (input) => {
    if (typeof input !== "object" || input === null || Array.isArray(input)) {
      return { ok: false, errors: [{ field: "", message: "must be an object" }] };
    }
    const record = input as Record<string, unknown>;
    const errors: ValidationError[] = [];
    const out: Record<string, unknown> = {};

    for (const [key, validator] of Object.entries(shape)) {
      const result = validator(record[key]);
      if (!result.ok) {
        for (const e of result.errors) {
          errors.push({ field: e.field ? `${key}.${e.field}` : key, message: e.message });
        }
      } else {
        out[key] = result.value;
      }
    }

    if (!options?.allowExtra) {
      for (const key of Object.keys(record)) {
        if (!(key in shape)) {
          errors.push({ field: key, message: "unexpected field" });
        }
      }
    }

    return errors.length > 0
      ? { ok: false, errors }
      : { ok: true, value: out as InferSchema<S>, errors: [] };
  };
}

function optional<T>(validator: Validator<T>): Validator<T | undefined> {
  return (input) => {
    if (input === undefined || input === null) {
      return { ok: true, value: undefined, errors: [] };
    }
    return validator(input) as ValidationResult<T | undefined>;
  };
}

export const v = { string, boolean, object, optional };

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

const PARAM_RE = /:([a-zA-Z_][a-zA-Z0-9_]*)/g;

function compilePattern(path: string): { pattern: RegExp; paramNames: string[] } {
  const paramNames: string[] = [];
  const escaped = path.replace(/[.*+?^${}()|[\]\\]/g, "\\$&").replace(PARAM_RE, (_, name) => {
    paramNames.push(name);
    return "([^/]+)";
  });
  return { pattern: new RegExp(`^${escaped}$`), paramNames };
}

export class Router {
  private routes: Route[] = [];
  private errorHandler: ErrorHandler = defaultErrorHandler;

  on(method: HttpMethod, path: string, handler: Handler): this {
    const { pattern, paramNames } = compilePattern(path);
    this.routes.push({ method, pattern, paramNames, handler });
    return this;
  }

  get(path: string, handler: Handler): this {
    return this.on("GET", path, handler);
  }
  post(path: string, handler: Handler): this {
    return this.on("POST", path, handler);
  }
  put(path: string, handler: Handler): this {
    return this.on("PUT", path, handler);
  }
  patch(path: string, handler: Handler): this {
    return this.on("PATCH", path, handler);
  }
  delete(path: string, handler: Handler): this {
    return this.on("DELETE", path, handler);
  }

  useErrorHandler(fn: ErrorHandler): this {
    this.errorHandler = fn;
    return this;
  }

  async handle(req: IncomingMessage, res: ServerResponse): Promise<void> {
    const tRes = extendResponse(res);
    const url = new URL(req.url ?? "/", "http://localhost");
    const pathname = url.pathname;
    const method = req.method?.toUpperCase() as HttpMethod;

    for (const route of this.routes) {
      if (route.method !== method) continue;
      const match = pathname.match(route.pattern);
      if (!match) continue;

      const params: RouteParams = {};
      route.paramNames.forEach((name, i) => {
        params[name] = decodeURIComponent(match[i + 1] ?? "");
      });

      let body: unknown = undefined;
      try {
        body = await readBody(req);
      } catch (err) {
        tRes.json({ error: (err as Error).message }, 400);
        return;
      }

      const tReq = Object.assign(req, {
        params,
        query: url.searchParams,
        body,
      }) as IncomingMessage & ParsedRequest;

      try {
        await route.handler(tReq, tRes);
      } catch (err) {
        try {
          await this.errorHandler(err, tReq, tRes);
        } catch {
          if (!res.headersSent) {
            tRes.json({ error: "Internal server error" }, 500);
          }
        }
      }
      return;
    }

    tRes.json({ error: "Not found" }, 404);
  }
}

// ---------------------------------------------------------------------------
// Response helper
// ---------------------------------------------------------------------------

function extendResponse(res: ServerResponse): TypedResponse {
  const tRes = res as TypedResponse;
  tRes.json = (data, status = 200) => {
    if (!res.headersSent) {
      const body = JSON.stringify(data);
      res.writeHead(status, {
        "Content-Type": "application/json",
        "Content-Length": Buffer.byteLength(body),
      });
      res.end(body);
    }
  };
  return tRes;
}

// ---------------------------------------------------------------------------
// Body parsing (JSON only, 100 KB limit)
// ---------------------------------------------------------------------------

const BODY_SIZE_LIMIT = 100 * 1024; // 100 KB

function readBody(req: IncomingMessage): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const contentType = req.headers["content-type"] ?? "";
    if (!contentType.includes("application/json")) {
      resolve(undefined);
      return;
    }

    let size = 0;
    const chunks: Buffer[] = [];

    req.on("data", (chunk: Buffer) => {
      size += chunk.length;
      if (size > BODY_SIZE_LIMIT) {
        req.destroy();
        reject(new Error("Request body too large (limit: 100 KB)"));
        return;
      }
      chunks.push(chunk);
    });

    req.on("end", () => {
      if (chunks.length === 0) {
        resolve(undefined);
        return;
      }
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString("utf8")));
      } catch {
        reject(new Error("Invalid JSON body"));
      }
    });

    req.on("error", reject);
  });
}

// ---------------------------------------------------------------------------
// Default error handler
// ---------------------------------------------------------------------------

function defaultErrorHandler(
  err: unknown,
  _req: IncomingMessage & ParsedRequest,
  res: TypedResponse
): void {
  const message = err instanceof Error ? err.message : "Internal server error";
  const status = (err as { status?: number }).status ?? 500;
  res.json({ error: message }, status);
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

class TodoStore {
  private items = new Map<string, Todo>();
  private counter = 0;

  list(): Todo[] {
    return [...this.items.values()].sort((a, b) =>
      a.createdAt.localeCompare(b.createdAt)
    );
  }

  get(id: string): Todo | undefined {
    return this.items.get(id);
  }

  create(title: string): Todo {
    const id = String(++this.counter);
    const now = new Date().toISOString();
    const todo: Todo = { id, title, completed: false, createdAt: now, updatedAt: now };
    this.items.set(id, todo);
    return todo;
  }

  update(id: string, patch: Partial<Pick<Todo, "title" | "completed">>): Todo | undefined {
    const todo = this.items.get(id);
    if (!todo) return undefined;
    const updated: Todo = { ...todo, ...patch, updatedAt: new Date().toISOString() };
    this.items.set(id, updated);
    return updated;
  }

  delete(id: string): boolean {
    return this.items.delete(id);
  }
}

const store = new TodoStore();

// ---------------------------------------------------------------------------
// Validation schemas
// ---------------------------------------------------------------------------

const createTodoSchema = v.object({ title: v.string({ minLength: 1, maxLength: 500 }) });

const updateTodoSchema = v.object(
  {
    title: v.optional(v.string({ minLength: 1, maxLength: 500 })),
    completed: v.optional(v.boolean()),
  },
  { allowExtra: false }
);

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

function notEmpty(patch: Partial<Pick<Todo, "title" | "completed">>): boolean {
  return patch.title !== undefined || patch.completed !== undefined;
}

const router = new Router();

router.get("/todos", (_req, res) => {
  res.json(store.list());
});

router.get("/todos/:id", (req, res) => {
  const todo = store.get(req.params.id);
  if (!todo) {
    res.json({ error: "Todo not found" }, 404);
    return;
  }
  res.json(todo);
});

router.post("/todos", (req, res) => {
  const result = createTodoSchema(req.body);
  if (!result.ok) {
    res.json({ error: "Validation failed", details: result.errors }, 422);
    return;
  }
  res.json(store.create(result.value!.title), 201);
});

router.patch("/todos/:id", (req, res) => {
  const result = updateTodoSchema(req.body);
  if (!result.ok) {
    res.json({ error: "Validation failed", details: result.errors }, 422);
    return;
  }

  const patch: Partial<Pick<Todo, "title" | "completed">> = {};
  if (result.value!.title !== undefined) patch.title = result.value!.title;
  if (result.value!.completed !== undefined) patch.completed = result.value!.completed;

  if (!notEmpty(patch)) {
    res.json({ error: "No updatable fields provided" }, 422);
    return;
  }

  const updated = store.update(req.params.id, patch);
  if (!updated) {
    res.json({ error: "Todo not found" }, 404);
    return;
  }
  res.json(updated);
});

router.delete("/todos/:id", (req, res) => {
  if (!store.delete(req.params.id)) {
    res.json({ error: "Todo not found" }, 404);
    return;
  }
  res.json(null, 204);
});

// ---------------------------------------------------------------------------
// Server bootstrap and graceful shutdown
// ---------------------------------------------------------------------------

const PORT = Number(process.env.PORT) || 3000;

const server = http.createServer((req, res) => {
  router.handle(req, res).catch(() => {
    if (!res.headersSent) {
      res.writeHead(500, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: "Internal server error" }));
    }
  });
});

function shutdown(signal: string): void {
  console.log(`Received ${signal}, shutting down gracefully…`);
  server.close((err) => {
    if (err) {
      console.error("Error during shutdown:", err);
      process.exit(1);
    }
    console.log("Server closed.");
    process.exit(0);
  });

  // Force exit if connections don't drain within 10 s
  setTimeout(() => {
    console.error("Forced shutdown after timeout.");
    process.exit(1);
  }, 10_000).unref();
}

process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT", () => shutdown("SIGINT"));

server.listen(PORT, () => {
  console.log(`Todo server listening on http://localhost:${PORT}`);
});

export { router, server };
