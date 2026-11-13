/**
 * server.ts — typed mini HTTP framework + todo CRUD, Node 22, no Express
 *
 * Run:  npx tsc && node dist/server.js
 * Dev:  npx tsx server.ts
 *
 * tsconfig requirements: "strict": true, "module": "NodeNext" (or "ESNext"),
 *   "target": "ES2022", "moduleResolution": "NodeNext"
 */

import http, { IncomingMessage, ServerResponse } from "node:http";

// ─── Core types ───────────────────────────────────────────────────────────────

type Params = Record<string, string>;
type Query  = Record<string, string | string[]>;

/** Augmented IncomingMessage with parsed fields attached by the router. */
interface Req extends IncomingMessage {
  params:   Params;
  query:    Query;
  body:     unknown;
  pathname: string;
}

/** Augmented ServerResponse with a typed json helper. */
interface Res extends ServerResponse {
  json(status: number, data: unknown): void;
}

type Handler      = (req: Req, res: Res) => Promise<void> | void;
type ErrorHandler = (err: unknown, req: Req, res: Res) => Promise<void> | void;

// ─── Schema validation ────────────────────────────────────────────────────────

type ValidationResult<T> =
  | { ok: true;  value: T }
  | { ok: false; errors: string[] };

type Validator<T> = (input: unknown) => ValidationResult<T>;

/** Narrow a SchemaShape into its inferred value type. */
type SchemaShape = Record<string, Validator<unknown>>;
type Infer<S extends SchemaShape> = {
  [K in keyof S]: S[K] extends Validator<infer T> ? T : never;
};

function strField(field: string): Validator<string> {
  return (input) => {
    if (typeof input === "string" && input.length > 0) {
      return { ok: true, value: input };
    }
    return { ok: false, errors: [`${field} must be a non-empty string`] };
  };
}

function boolField(field: string): Validator<boolean> {
  return (input) => {
    if (typeof input === "boolean") {
      return { ok: true, value: input };
    }
    return { ok: false, errors: [`${field} must be a boolean`] };
  };
}

function optional<T>(validator: Validator<T>): Validator<T | undefined> {
  return (input) => {
    if (input === undefined || input === null) {
      return { ok: true, value: undefined };
    }
    // The inner validator returns ValidationResult<T>; widening to T | undefined is safe.
    return validator(input) as ValidationResult<T | undefined>;
  };
}

function objectOf<S extends SchemaShape>(shape: S): Validator<Infer<S>> {
  return (input) => {
    if (
      typeof input !== "object" ||
      input === null ||
      Array.isArray(input)
    ) {
      return { ok: false, errors: ["body must be a JSON object"] };
    }

    const record = input as Record<string, unknown>;
    const result: Record<string, unknown> = {};
    const errors: string[] = [];

    for (const key of Object.keys(shape) as Array<keyof S & string>) {
      const v = shape[key]!(record[key]);
      if (v.ok) {
        result[key] = v.value;
      } else {
        errors.push(...v.errors);
      }
    }

    return errors.length > 0
      ? { ok: false, errors }
      : { ok: true, value: result as Infer<S> };
  };
}

// ─── Application error ────────────────────────────────────────────────────────

class AppError extends Error {
  constructor(
    public readonly status: number,
    message: string,
  ) {
    super(message);
    this.name = "AppError";
  }
}

// ─── Body parser ──────────────────────────────────────────────────────────────

const MAX_BODY_BYTES = 1_048_576; // 1 MiB

function parseBody(req: IncomingMessage): Promise<unknown> {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks: Buffer[] = [];

    req.on("data", (chunk: Buffer) => {
      size += chunk.length;
      if (size > MAX_BODY_BYTES) {
        req.destroy(
          new AppError(413, `Request body exceeds ${MAX_BODY_BYTES} bytes`),
        );
        return;
      }
      chunks.push(chunk);
    });

    req.on("end", () => {
      const raw = Buffer.concat(chunks).toString("utf8").trim();
      if (!raw) {
        resolve(undefined);
        return;
      }
      try {
        resolve(JSON.parse(raw));
      } catch {
        reject(new AppError(400, "Request body contains invalid JSON"));
      }
    });

    req.on("error", (err) => {
      // Surface the AppError we attached via req.destroy(), or wrap unknown errors.
      reject(err instanceof AppError ? err : new AppError(400, "Failed to read request body"));
    });
  });
}

// ─── Router ───────────────────────────────────────────────────────────────────

interface Route {
  method:     string;
  pattern:    RegExp;
  paramNames: string[];
  handler:    Handler;
}

class Router {
  private readonly routes: Route[] = [];
  private errorHandler: ErrorHandler | null = null;

  /** Compile a path like /todos/:id into a RegExp + param name list. */
  private compile(path: string): { pattern: RegExp; paramNames: string[] } {
    const paramNames: string[] = [];
    // Escape everything except the leading slash, then replace :param tokens.
    const src = path
      .split("/")
      .map((seg) =>
        seg.startsWith(":")
          ? (() => { paramNames.push(seg.slice(1)); return "([^/]+)"; })()
          : seg.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"),
      )
      .join("/");
    return { pattern: new RegExp(`^${src}$`), paramNames };
  }

  add(method: string, path: string, handler: Handler): this {
    const { pattern, paramNames } = this.compile(path);
    this.routes.push({ method: method.toUpperCase(), pattern, paramNames, handler });
    return this;
  }

  get(path: string, handler: Handler): this    { return this.add("GET",    path, handler); }
  post(path: string, handler: Handler): this   { return this.add("POST",   path, handler); }
  put(path: string, handler: Handler): this    { return this.add("PUT",    path, handler); }
  patch(path: string, handler: Handler): this  { return this.add("PATCH",  path, handler); }
  delete(path: string, handler: Handler): this { return this.add("DELETE", path, handler); }

  onError(handler: ErrorHandler): this {
    this.errorHandler = handler;
    return this;
  }

  /** Returns a standard Node http handler. */
  handler(): (req: IncomingMessage, res: ServerResponse) => void {
    return (rawReq, rawRes) => {
      // Augment req/res before dispatch.
      const req = rawReq as Req;
      const res = rawRes as Res;

      res.json = (status, data) => {
        if (res.headersSent) return;
        const body = JSON.stringify(data);
        res.writeHead(status, {
          "Content-Type":   "application/json; charset=utf-8",
          "Content-Length": Buffer.byteLength(body),
        });
        res.end(body);
      };

      void this.dispatch(req, res);
    };
  }

  private async dispatch(req: Req, res: Res): Promise<void> {
    const url = new URL(
      req.url ?? "/",
      `http://${req.headers.host ?? "localhost"}`,
    );

    req.pathname = url.pathname;
    req.params   = {};
    req.query    = {};
    req.body     = undefined;

    // Accumulate query params; repeated keys become arrays.
    url.searchParams.forEach((value, key) => {
      const existing = req.query[key];
      if (existing === undefined) {
        req.query[key] = value;
      } else if (Array.isArray(existing)) {
        existing.push(value);
      } else {
        req.query[key] = [existing, value];
      }
    });

    const method = (req.method ?? "GET").toUpperCase();

    // Parse body for methods that carry one.
    if (method === "POST" || method === "PUT" || method === "PATCH") {
      try {
        req.body = await parseBody(req);
      } catch (err) {
        const status = err instanceof AppError ? err.status : 400;
        const message = err instanceof Error ? err.message : "Bad request";
        res.json(status, { error: message });
        return;
      }
    }

    // Find first matching route.
    for (const route of this.routes) {
      if (route.method !== method) continue;
      const match = route.pattern.exec(req.pathname);
      if (!match) continue;

      for (let i = 0; i < route.paramNames.length; i++) {
        req.params[route.paramNames[i]!] = decodeURIComponent(match[i + 1]!);
      }

      try {
        await route.handler(req, res);
      } catch (err) {
        await this.handleError(err, req, res);
      }
      return;
    }

    // No match → 404.
    res.json(404, { error: `Cannot ${method} ${req.pathname}` });
  }

  private async handleError(err: unknown, req: Req, res: Res): Promise<void> {
    if (this.errorHandler) {
      try {
        await this.errorHandler(err, req, res);
        return;
      } catch (inner) {
        console.error("[router] error handler threw:", inner);
      }
    }
    // Final fallback if no error handler was registered or it threw.
    res.json(500, { error: "Internal server error" });
  }
}

// ─── Error middleware ─────────────────────────────────────────────────────────

function errorMiddleware(err: unknown, _req: Req, res: Res): void {
  if (err instanceof AppError) {
    res.json(err.status, { error: err.message });
    return;
  }
  const message = err instanceof Error ? err.message : "Internal server error";
  console.error("[error]", err);
  res.json(500, { error: message });
}

// ─── Todo resource ────────────────────────────────────────────────────────────

interface Todo {
  id:        string;
  title:     string;
  done:      boolean;
  createdAt: string;
  updatedAt: string;
}

// In-memory store — keyed by id string.
const todoStore = new Map<string, Todo>();
let   nextId    = 1;

function newId(): string {
  return String(nextId++);
}

// Validation schemas.
const createSchema = objectOf({
  title: strField("title"),
  done:  optional(boolField("done")),
});

const updateSchema = objectOf({
  title: optional(strField("title")),
  done:  optional(boolField("done")),
});

// Handlers.

function listTodos(_req: Req, res: Res): void {
  res.json(200, Array.from(todoStore.values()));
}

function getTodo(req: Req, res: Res): void {
  const todo = todoStore.get(req.params["id"]!);
  if (!todo) throw new AppError(404, `Todo "${req.params["id"]}" not found`);
  res.json(200, todo);
}

function createTodo(req: Req, res: Res): void {
  const result = createSchema(req.body);
  if (!result.ok) throw new AppError(422, result.errors.join("; "));

  const now  = new Date().toISOString();
  const todo: Todo = {
    id:        newId(),
    title:     result.value.title,
    done:      result.value.done ?? false,
    createdAt: now,
    updatedAt: now,
  };
  todoStore.set(todo.id, todo);
  res.json(201, todo);
}

function updateTodo(req: Req, res: Res): void {
  const id       = req.params["id"]!;
  const existing = todoStore.get(id);
  if (!existing) throw new AppError(404, `Todo "${id}" not found`);

  const result = updateSchema(req.body);
  if (!result.ok) throw new AppError(422, result.errors.join("; "));

  const { title, done } = result.value;
  const updated: Todo = {
    ...existing,
    ...(title !== undefined && { title }),
    ...(done  !== undefined && { done  }),
    updatedAt: new Date().toISOString(),
  };
  todoStore.set(id, updated);
  res.json(200, updated);
}

function deleteTodo(req: Req, res: Res): void {
  const id = req.params["id"]!;
  if (!todoStore.delete(id)) {
    throw new AppError(404, `Todo "${id}" not found`);
  }
  res.json(200, { deleted: id });
}

// ─── Wiring ───────────────────────────────────────────────────────────────────

const router = new Router();

router
  .get   ("/todos",     listTodos)
  .post  ("/todos",     createTodo)
  .get   ("/todos/:id", getTodo)
  .patch ("/todos/:id", updateTodo)
  .delete("/todos/:id", deleteTodo)
  .onError(errorMiddleware);

const PORT   = Number(process.env["PORT"] ?? 3000);
const server = http.createServer(router.handler());

server.listen(PORT, () => {
  console.log(`[server] listening on http://localhost:${PORT}`);
});

// ─── Graceful shutdown ────────────────────────────────────────────────────────

function shutdown(signal: string): void {
  console.log(`\n[server] ${signal} — closing`);

  server.close((err) => {
    if (err) {
      console.error("[server] shutdown error:", err);
      process.exit(1);
    }
    console.log("[server] closed");
    process.exit(0);
  });

  // Force-exit if open connections stall beyond 10 s.
  setTimeout(() => {
    console.error("[server] forced shutdown after timeout");
    process.exit(1);
  }, 10_000).unref();
}

process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT",  () => shutdown("SIGINT"));
