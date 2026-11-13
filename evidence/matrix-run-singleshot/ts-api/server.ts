/**
 * server.ts — typed mini HTTP framework with router, validation, in-memory
 * todo CRUD, and graceful shutdown. Node 22, no external dependencies.
 *
 * Run:  npx ts-node --transpile-only server.ts
 * Build: tsc && node server.js
 */

import http, { IncomingMessage, ServerResponse } from "node:http";

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

type Method = "GET" | "POST" | "PUT" | "PATCH" | "DELETE";

interface RouteParams {
  [key: string]: string;
}

interface ParsedRequest {
  params: RouteParams;
  query: URLSearchParams;
  body: unknown;
}

type Handler = (
  req: IncomingMessage & ParsedRequest,
  res: TypedResponse
) => void | Promise<void>;

type Middleware = (
  req: IncomingMessage & ParsedRequest,
  res: TypedResponse,
  next: (err?: unknown) => void
) => void | Promise<void>;

// ---------------------------------------------------------------------------
// TypedResponse — thin wrapper around ServerResponse
// ---------------------------------------------------------------------------

class TypedResponse {
  private sent = false;

  constructor(private readonly raw: ServerResponse) {}

  status(code: number): this {
    this.raw.statusCode = code;
    return this;
  }

  json(data: unknown): void {
    if (this.sent) return;
    this.sent = true;
    const body = JSON.stringify(data);
    this.raw.setHeader("Content-Type", "application/json");
    this.raw.setHeader("Content-Length", Buffer.byteLength(body));
    this.raw.end(body);
  }

  text(data: string, code = 200): void {
    if (this.sent) return;
    this.sent = true;
    this.raw.statusCode = code;
    this.raw.setHeader("Content-Type", "text/plain");
    this.raw.end(data);
  }
}

// ---------------------------------------------------------------------------
// Schema validation helpers
// ---------------------------------------------------------------------------

type SchemaType = "string" | "number" | "boolean";

interface FieldRule {
  type: SchemaType;
  required?: boolean;
  minLength?: number;
  maxLength?: number;
  min?: number;
  max?: number;
}

type Schema = Record<string, FieldRule>;

interface ValidationResult {
  ok: boolean;
  errors: string[];
}

function validate(data: unknown, schema: Schema): ValidationResult {
  const errors: string[] = [];

  if (typeof data !== "object" || data === null || Array.isArray(data)) {
    return { ok: false, errors: ["Request body must be a JSON object"] };
  }

  const obj = data as Record<string, unknown>;

  for (const [field, rule] of Object.entries(schema)) {
    const value = obj[field];

    if (value === undefined || value === null) {
      if (rule.required) errors.push(`"${field}" is required`);
      continue;
    }

    // Type check
    // eslint-disable-next-line valid-typeof
    if (typeof value !== rule.type) {
      errors.push(`"${field}" must be of type ${rule.type}`);
      continue;
    }

    if (rule.type === "string") {
      const s = value as string;
      if (rule.minLength !== undefined && s.length < rule.minLength)
        errors.push(`"${field}" must be at least ${rule.minLength} characters`);
      if (rule.maxLength !== undefined && s.length > rule.maxLength)
        errors.push(`"${field}" must be at most ${rule.maxLength} characters`);
    }

    if (rule.type === "number") {
      const n = value as number;
      if (rule.min !== undefined && n < rule.min)
        errors.push(`"${field}" must be >= ${rule.min}`);
      if (rule.max !== undefined && n > rule.max)
        errors.push(`"${field}" must be <= ${rule.max}`);
    }
  }

  return { ok: errors.length === 0, errors };
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

interface Route {
  method: Method;
  pattern: RegExp;
  paramNames: string[];
  handler: Handler;
}

class Router {
  private readonly routes: Route[] = [];
  private readonly middlewares: Middleware[] = [];

  use(mw: Middleware): void {
    this.middlewares.push(mw);
  }

  private register(method: Method, path: string, handler: Handler): void {
    // Convert :param segments into named capture groups
    const paramNames: string[] = [];
    const regexStr = path
      .replace(/\//g, "\\/")
      .replace(/:([A-Za-z_][A-Za-z0-9_]*)/g, (_match, name: string) => {
        paramNames.push(name);
        return "([^/]+)";
      });
    const pattern = new RegExp(`^${regexStr}$`);
    this.routes.push({ method, pattern, paramNames, handler });
  }

  get(path: string, handler: Handler): void {
    this.register("GET", path, handler);
  }
  post(path: string, handler: Handler): void {
    this.register("POST", path, handler);
  }
  put(path: string, handler: Handler): void {
    this.register("PUT", path, handler);
  }
  patch(path: string, handler: Handler): void {
    this.register("PATCH", path, handler);
  }
  delete(path: string, handler: Handler): void {
    this.register("DELETE", path, handler);
  }

  async dispatch(
    req: IncomingMessage,
    res: ServerResponse,
    body: unknown
  ): Promise<void> {
    const url = new URL(req.url ?? "/", "http://localhost");
    const pathname = url.pathname;
    const method = (req.method ?? "GET").toUpperCase() as Method;

    const typedRes = new TypedResponse(res);

    // Find matching route
    let matchedRoute: Route | undefined;
    let params: RouteParams = {};

    for (const route of this.routes) {
      if (route.method !== method) continue;
      const m = pathname.match(route.pattern);
      if (!m) continue;
      matchedRoute = route;
      params = {};
      route.paramNames.forEach((name, i) => {
        params[name] = decodeURIComponent(m[i + 1] ?? "");
      });
      break;
    }

    if (!matchedRoute) {
      typedRes.status(404).json({ error: "Not found" });
      return;
    }

    // Attach parsed data to the request object
    const parsedReq = req as IncomingMessage & ParsedRequest;
    parsedReq.params = params;
    parsedReq.query = url.searchParams;
    parsedReq.body = body;

    // Run middleware chain, then the route handler
    const chain: Array<Middleware | Handler> = [
      ...this.middlewares,
      matchedRoute.handler,
    ];
    let idx = 0;

    const next = async (err?: unknown): Promise<void> => {
      if (err !== undefined) {
        // Let error middleware or the global handler deal with it
        typedRes.status(500).json({
          error: err instanceof Error ? err.message : "Internal server error",
        });
        return;
      }
      const fn = chain[idx++];
      if (!fn) return;
      try {
        if (idx <= chain.length) {
          await (fn as Middleware)(parsedReq, typedRes, next);
        }
      } catch (e) {
        await next(e);
      }
    };

    await next();
  }
}

// ---------------------------------------------------------------------------
// Body parser (JSON only, with size limit)
// ---------------------------------------------------------------------------

const BODY_SIZE_LIMIT = 100 * 1024; // 100 KB

function readBody(req: IncomingMessage): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const contentType = req.headers["content-type"] ?? "";

    if (!contentType.includes("application/json")) {
      resolve(null);
      return;
    }

    let total = 0;
    const chunks: Buffer[] = [];

    req.on("data", (chunk: Buffer) => {
      total += chunk.length;
      if (total > BODY_SIZE_LIMIT) {
        req.destroy(new Error("Payload too large"));
        return;
      }
      chunks.push(chunk);
    });

    req.on("end", () => {
      if (chunks.length === 0) {
        resolve(null);
        return;
      }
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString("utf8")));
      } catch {
        reject(new Error("Invalid JSON"));
      }
    });

    req.on("error", reject);
  });
}

// ---------------------------------------------------------------------------
// In-memory todo store
// ---------------------------------------------------------------------------

interface Todo {
  id: string;
  title: string;
  completed: boolean;
  createdAt: string;
  updatedAt: string;
}

const todos = new Map<string, Todo>();
let nextId = 1;

function newId(): string {
  return String(nextId++);
}

// Schema for creating a todo
const createSchema: Schema = {
  title: { type: "string", required: true, minLength: 1, maxLength: 256 },
};

// Schema for updating a todo
const updateSchema: Schema = {
  title: { type: "string", required: false, minLength: 1, maxLength: 256 },
  completed: { type: "boolean", required: false },
};

// ---------------------------------------------------------------------------
// Application setup
// ---------------------------------------------------------------------------

const router = new Router();

// --- GET /todos
router.get("/todos", (_req, res) => {
  res.status(200).json(Array.from(todos.values()));
});

// --- GET /todos/:id
router.get("/todos/:id", (req, res) => {
  const todo = todos.get(req.params.id);
  if (!todo) {
    res.status(404).json({ error: "Todo not found" });
    return;
  }
  res.status(200).json(todo);
});

// --- POST /todos
router.post("/todos", (req, res) => {
  const result = validate(req.body, createSchema);
  if (!result.ok) {
    res.status(422).json({ error: "Validation failed", details: result.errors });
    return;
  }

  const body = req.body as { title: string };
  const now = new Date().toISOString();
  const todo: Todo = {
    id: newId(),
    title: body.title.trim(),
    completed: false,
    createdAt: now,
    updatedAt: now,
  };
  todos.set(todo.id, todo);
  res.status(201).json(todo);
});

// --- PUT /todos/:id  (full replace)
router.put("/todos/:id", (req, res) => {
  const existing = todos.get(req.params.id);
  if (!existing) {
    res.status(404).json({ error: "Todo not found" });
    return;
  }

  const result = validate(req.body, {
    title: { type: "string", required: true, minLength: 1, maxLength: 256 },
    completed: { type: "boolean", required: true },
  });
  if (!result.ok) {
    res.status(422).json({ error: "Validation failed", details: result.errors });
    return;
  }

  const body = req.body as { title: string; completed: boolean };
  const updated: Todo = {
    ...existing,
    title: body.title.trim(),
    completed: body.completed,
    updatedAt: new Date().toISOString(),
  };
  todos.set(updated.id, updated);
  res.status(200).json(updated);
});

// --- PATCH /todos/:id  (partial update)
router.patch("/todos/:id", (req, res) => {
  const existing = todos.get(req.params.id);
  if (!existing) {
    res.status(404).json({ error: "Todo not found" });
    return;
  }

  const result = validate(req.body, updateSchema);
  if (!result.ok) {
    res.status(422).json({ error: "Validation failed", details: result.errors });
    return;
  }

  const body = req.body as Partial<Pick<Todo, "title" | "completed">>;
  const updated: Todo = {
    ...existing,
    ...(body.title !== undefined ? { title: body.title.trim() } : {}),
    ...(body.completed !== undefined ? { completed: body.completed } : {}),
    updatedAt: new Date().toISOString(),
  };
  todos.set(updated.id, updated);
  res.status(200).json(updated);
});

// --- DELETE /todos/:id
router.delete("/todos/:id", (req, res) => {
  if (!todos.has(req.params.id)) {
    res.status(404).json({ error: "Todo not found" });
    return;
  }
  todos.delete(req.params.id);
  res.status(204).json(null);
});

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------

const PORT = Number(process.env.PORT ?? 3000);

const server = http.createServer(async (req, res) => {
  // Global error shape for uncaught throws at the top level
  const bail = (code: number, message: string): void => {
    if (!res.headersSent) {
      res.writeHead(code, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: message }));
    }
  };

  try {
    const body = await readBody(req).catch((e: unknown) => {
      const msg = e instanceof Error ? e.message : "Bad request";
      // 413 for size, 400 for malformed JSON
      const code = msg === "Payload too large" ? 413 : 400;
      bail(code, msg);
      return undefined;
    });

    // If bail() was already called by readBody, the response is done
    if (res.headersSent) return;

    await router.dispatch(req, res, body);

    // Catch-all 404 for anything the router didn't match
    if (!res.writableEnded) {
      bail(404, "Not found");
    }
  } catch (err) {
    console.error("Unhandled error:", err);
    bail(500, err instanceof Error ? err.message : "Internal server error");
  }
});

server.listen(PORT, () => {
  console.log(`Server listening on http://localhost:${PORT}`);
});

// ---------------------------------------------------------------------------
// Graceful shutdown
// ---------------------------------------------------------------------------

let shuttingDown = false;

function shutdown(signal: string): void {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log(`\nReceived ${signal}. Closing server...`);

  server.close((err) => {
    if (err) {
      console.error("Error during shutdown:", err);
      process.exit(1);
    }
    console.log("Server closed. Goodbye.");
    process.exit(0);
  });

  // Force exit if connections don't drain within 10 s
  setTimeout(() => {
    console.error("Forcing exit after timeout.");
    process.exit(1);
  }, 10_000).unref();
}

process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT", () => shutdown("SIGINT"));
