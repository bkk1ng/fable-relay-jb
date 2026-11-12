import http from "node:http";
import { IncomingMessage, ServerResponse } from "node:http";

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

export type HttpMethod = "GET" | "POST" | "PUT" | "PATCH" | "DELETE" | "HEAD" | "OPTIONS";

export interface Request extends IncomingMessage {
  params: Record<string, string>;
  query: Record<string, string>;
  body: unknown;
}

export interface Response extends ServerResponse {
  json: (statusCode: number, data: unknown) => void;
  sendError: (statusCode: number, message: string, details?: unknown) => void;
}

export type Handler = (req: Request, res: Response) => Promise<void> | void;
export type ErrorHandler = (err: unknown, req: Request, res: Response) => Promise<void> | void;
export type Middleware = (req: Request, res: Response, next: (err?: unknown) => void) => void;

interface Route {
  method: HttpMethod;
  pattern: RegExp;
  paramNames: string[];
  handler: Handler;
}

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

export type SchemaType = "string" | "number" | "boolean" | "object" | "array";

export interface FieldSchema {
  type: SchemaType;
  required?: boolean;
  minLength?: number;
  maxLength?: number;
  min?: number;
  max?: number;
}

export type Schema = Record<string, FieldSchema>;

export interface ValidationResult {
  valid: boolean;
  errors: string[];
}

export function validate(data: unknown, schema: Schema): ValidationResult {
  const errors: string[] = [];

  if (typeof data !== "object" || data === null || Array.isArray(data)) {
    return { valid: false, errors: ["Request body must be a JSON object"] };
  }

  const obj = data as Record<string, unknown>;

  for (const [field, rules] of Object.entries(schema)) {
    const value = obj[field];
    const missing = value === undefined || value === null;

    if (missing) {
      if (rules.required) errors.push(`"${field}" is required`);
      continue;
    }

    const actualType = Array.isArray(value) ? "array" : typeof value;
    if (actualType !== rules.type) {
      errors.push(`"${field}" must be of type ${rules.type}, got ${actualType}`);
      continue;
    }

    if (rules.type === "string" && typeof value === "string") {
      if (rules.minLength !== undefined && value.length < rules.minLength)
        errors.push(`"${field}" must be at least ${rules.minLength} characters`);
      if (rules.maxLength !== undefined && value.length > rules.maxLength)
        errors.push(`"${field}" must be at most ${rules.maxLength} characters`);
    }

    if (rules.type === "number" && typeof value === "number") {
      if (rules.min !== undefined && value < rules.min)
        errors.push(`"${field}" must be >= ${rules.min}`);
      if (rules.max !== undefined && value > rules.max)
        errors.push(`"${field}" must be <= ${rules.max}`);
    }
  }

  return { valid: errors.length === 0, errors };
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

function pathToRegex(path: string): { pattern: RegExp; paramNames: string[] } {
  const paramNames: string[] = [];
  const regexStr = path
    .replace(/[.*+?^${}()|[\]\\]/g, (c) => (c === ":" ? c : `\\${c}`))
    .replace(/:([a-zA-Z_][a-zA-Z0-9_]*)/g, (_match, name: string) => {
      paramNames.push(name);
      return "([^/]+)";
    });
  return { pattern: new RegExp(`^${regexStr}$`), paramNames };
}

function parseQuery(url: string): Record<string, string> {
  const idx = url.indexOf("?");
  if (idx === -1) return {};
  return Object.fromEntries(new URLSearchParams(url.slice(idx + 1)));
}

function parsePathname(url: string): string {
  const idx = url.indexOf("?");
  return idx === -1 ? url : url.slice(0, idx);
}

class Router {
  private routes: Route[] = [];
  private errorHandler: ErrorHandler = defaultErrorHandler;
  private readonly bodySizeLimit: number;

  constructor(bodySizeLimitBytes = 1024 * 1024 /* 1 MB */) {
    this.bodySizeLimit = bodySizeLimitBytes;
  }

  private addRoute(method: HttpMethod, path: string, handler: Handler): this {
    const { pattern, paramNames } = pathToRegex(path);
    this.routes.push({ method, pattern, paramNames, handler });
    return this;
  }

  get(path: string, handler: Handler): this { return this.addRoute("GET", path, handler); }
  post(path: string, handler: Handler): this { return this.addRoute("POST", path, handler); }
  put(path: string, handler: Handler): this { return this.addRoute("PUT", path, handler); }
  patch(path: string, handler: Handler): this { return this.addRoute("PATCH", path, handler); }
  delete(path: string, handler: Handler): this { return this.addRoute("DELETE", path, handler); }

  onError(handler: ErrorHandler): this {
    this.errorHandler = handler;
    return this;
  }

  private async readBody(req: IncomingMessage): Promise<unknown> {
    return new Promise((resolve, reject) => {
      const chunks: Buffer[] = [];
      let size = 0;

      req.on("data", (chunk: Buffer) => {
        size += chunk.length;
        if (size > this.bodySizeLimit) {
          req.destroy();
          reject(Object.assign(new Error("Request body too large"), { statusCode: 413 }));
          return;
        }
        chunks.push(chunk);
      });

      req.on("end", () => {
        const raw = Buffer.concat(chunks).toString("utf8");
        if (!raw) { resolve(undefined); return; }
        try {
          resolve(JSON.parse(raw));
        } catch {
          reject(Object.assign(new Error("Invalid JSON in request body"), { statusCode: 400 }));
        }
      });

      req.on("error", reject);
    });
  }

  private enhance(req: IncomingMessage, res: ServerResponse): [Request, Response] {
    const request = req as Request;
    request.params = {};
    request.query = parseQuery(req.url ?? "/");
    request.body = undefined;

    const response = res as Response;

    response.json = (statusCode, data) => {
      const body = JSON.stringify(data);
      res.writeHead(statusCode, {
        "Content-Type": "application/json; charset=utf-8",
        "Content-Length": Buffer.byteLength(body),
      });
      res.end(body);
    };

    response.sendError = (statusCode, message, details?) => {
      response.json(statusCode, { error: message, ...(details !== undefined && { details }) });
    };

    return [request, response];
  }

  handler(): (req: IncomingMessage, res: ServerResponse) => void {
    return async (rawReq, rawRes) => {
      const [req, res] = this.enhance(rawReq, rawRes);

      try {
        const pathname = parsePathname(req.url ?? "/");
        const method = req.method as HttpMethod;

        const matched = this.routes.find(
          (r) => r.method === method && r.pattern.test(pathname)
        );

        if (!matched) {
          res.sendError(404, `Cannot ${method} ${pathname}`);
          return;
        }

        // Parse params
        const match = pathname.match(matched.pattern);
        if (match) {
          matched.paramNames.forEach((name, i) => {
            req.params[name] = decodeURIComponent(match[i + 1] ?? "");
          });
        }

        // Parse body for methods that carry one
        if (["POST", "PUT", "PATCH"].includes(method)) {
          req.body = await this.readBody(req);
        }

        await matched.handler(req, res);
      } catch (err) {
        try {
          await this.errorHandler(err, req, res);
        } catch {
          if (!res.headersSent) {
            res.sendError(500, "Internal server error");
          }
        }
      }
    };
  }
}

// ---------------------------------------------------------------------------
// Default error handler
// ---------------------------------------------------------------------------

function defaultErrorHandler(err: unknown, _req: Request, res: Response): void {
  if (res.headersSent) return;

  const statusCode =
    typeof err === "object" && err !== null && "statusCode" in err
      ? (err as { statusCode: number }).statusCode
      : 500;

  const message =
    err instanceof Error ? err.message : "Internal server error";

  res.sendError(statusCode, message);
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

interface TodoStore {
  findAll(): Todo[];
  findById(id: string): Todo | undefined;
  create(title: string): Todo;
  update(id: string, patch: Partial<Pick<Todo, "title" | "completed">>): Todo | undefined;
  remove(id: string): boolean;
}

function createTodoStore(): TodoStore {
  const store = new Map<string, Todo>();
  let counter = 0;

  return {
    findAll: () => Array.from(store.values()),

    findById: (id) => store.get(id),

    create: (title) => {
      const id = String(++counter);
      const now = new Date().toISOString();
      const todo: Todo = { id, title, completed: false, createdAt: now, updatedAt: now };
      store.set(id, todo);
      return todo;
    },

    update: (id, patch) => {
      const existing = store.get(id);
      if (!existing) return undefined;
      const updated: Todo = { ...existing, ...patch, updatedAt: new Date().toISOString() };
      store.set(id, updated);
      return updated;
    },

    remove: (id) => {
      if (!store.has(id)) return false;
      store.delete(id);
      return true;
    },
  };
}

// ---------------------------------------------------------------------------
// Todo route schemas
// ---------------------------------------------------------------------------

const createTodoSchema: Schema = {
  title: { type: "string", required: true, minLength: 1, maxLength: 500 },
};

const updateTodoSchema: Schema = {
  title: { type: "string", minLength: 1, maxLength: 500 },
  completed: { type: "boolean" },
};

// ---------------------------------------------------------------------------
// Application bootstrap
// ---------------------------------------------------------------------------

function buildApp(): Router {
  const router = new Router(512 * 1024 /* 512 KB body limit */);
  const todos = createTodoStore();

  // Health
  router.get("/health", (_req, res) => {
    res.json(200, { status: "ok" });
  });

  // List all todos
  router.get("/todos", (_req, res) => {
    res.json(200, todos.findAll());
  });

  // Get single todo
  router.get("/todos/:id", (req, res) => {
    const todo = todos.findById(req.params.id);
    if (!todo) { res.sendError(404, `Todo ${req.params.id} not found`); return; }
    res.json(200, todo);
  });

  // Create todo
  router.post("/todos", (req, res) => {
    const result = validate(req.body, createTodoSchema);
    if (!result.valid) { res.sendError(400, "Validation failed", result.errors); return; }

    const { title } = req.body as { title: string };
    const todo = todos.create(title.trim());
    res.json(201, todo);
  });

  // Replace todo (full update)
  router.put("/todos/:id", (req, res) => {
    if (!todos.findById(req.params.id)) {
      res.sendError(404, `Todo ${req.params.id} not found`);
      return;
    }
    const result = validate(req.body, createTodoSchema);
    if (!result.valid) { res.sendError(400, "Validation failed", result.errors); return; }

    const { title } = req.body as { title: string };
    const todo = todos.update(req.params.id, { title: title.trim(), completed: false });
    res.json(200, todo);
  });

  // Partial update
  router.patch("/todos/:id", (req, res) => {
    if (!todos.findById(req.params.id)) {
      res.sendError(404, `Todo ${req.params.id} not found`);
      return;
    }
    const result = validate(req.body, updateTodoSchema);
    if (!result.valid) { res.sendError(400, "Validation failed", result.errors); return; }

    const patch = req.body as Partial<Pick<Todo, "title" | "completed">>;
    if (patch.title !== undefined) patch.title = patch.title.trim();
    const todo = todos.update(req.params.id, patch);
    res.json(200, todo);
  });

  // Delete todo
  router.delete("/todos/:id", (req, res) => {
    const removed = todos.remove(req.params.id);
    if (!removed) { res.sendError(404, `Todo ${req.params.id} not found`); return; }
    res.json(204, null);
  });

  // Custom error handler
  router.onError((err, _req, res) => {
    if (res.headersSent) return;

    if (typeof err === "object" && err !== null && "statusCode" in err) {
      const { statusCode, message } = err as { statusCode: number; message: string };
      res.sendError(statusCode, message);
      return;
    }

    console.error("[error]", err);
    res.sendError(500, "Internal server error");
  });

  return router;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

const PORT = Number(process.env.PORT ?? 3000);
const HOST = process.env.HOST ?? "127.0.0.1";

const router = buildApp();
const server = http.createServer(router.handler());

server.listen(PORT, HOST, () => {
  console.log(`listening on http://${HOST}:${PORT}`);
});

// Graceful shutdown
function shutdown(signal: string): void {
  console.log(`\n${signal} received — shutting down`);
  server.close((err) => {
    if (err) {
      console.error("error during shutdown:", err);
      process.exit(1);
    }
    console.log("server closed");
    process.exit(0);
  });
}

process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT", () => shutdown("SIGINT"));
