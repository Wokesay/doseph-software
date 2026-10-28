// SPDX-License-Identifier: AGPL-3.0-or-later
// The OpenAPI file (docs/openapi.json) describes every route of the API and
// nothing else (#10 step 7, SD-129). Which roles may call an operation is
// checked against the device's own rule by the core tests (test_api.cpp).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const spec = JSON.parse(readFileSync(join(root, "docs/openapi.json"), "utf8"));
const api = readFileSync(join(root, "core/src/api.cpp"), "utf8");
const server = readFileSync(join(root, "sim/main.cpp"), "utf8");
const METHODS = ["get", "put", "post", "delete", "patch"];
const ROLES = ["owner", "app", "account", "peer"];

// "GET /plants/{}/next" for each is("GET", {"plants", "*", "next"}) of the router
export function routesOf(source) {
  const out = [];
  for (const m of source.matchAll(/is\("([A-Z]+)", \{([^}]*)\}\)/g)) {
    const parts = [...m[2].matchAll(/"([^"]*)"/g)].map((p) => (p[1] === "*" ? "{}" : p[1]));
    out.push(`${m[1]} /${parts.join("/")}`);
  }
  return out;
}

const operations = () => {
  const out = [];
  for (const [path, item] of Object.entries(spec.paths))
    for (const method of METHODS) if (item[method]) out.push({ key: `${method.toUpperCase()} ${path.replace(/\{[^}]+\}/g, "{}")}`, op: item[method] });
  return out;
};

test("every route of the API is documented, and nothing else", () => {
  const routes = routesOf(api);
  assert.ok(routes.length > 50, "the router's routes are found");
  // The event stream is served by the platform's transport, on the device as in the simulator
  assert.match(server, /svr\.Get\("\/api\/v1\/events\/stream"/);
  routes.push("GET /events/stream");
  const documented = operations().map((o) => o.key);
  assert.deepEqual([...documented].sort(), [...new Set(routes)].sort());
});

test("the public operations are those the router answers before signing in", () => {
  const cut = api.indexOf("// ---- signed in only");
  assert.ok(cut > 0);
  const pub = routesOf(api.slice(0, cut)).sort();
  const open = operations().filter((o) => Array.isArray(o.op.security) && o.op.security.length === 0).map((o) => o.key).sort();
  assert.deepEqual(open, pub);
});

test("each operation has an id, a summary, a tag and its roles", () => {
  const ids = new Set();
  const tags = new Set(spec.tags.map((t) => t.name));
  for (const { key, op } of operations()) {
    assert.ok(op.operationId && !ids.has(op.operationId), `${key}: a unique operationId`);
    ids.add(op.operationId);
    assert.ok(op.summary, `${key}: a summary`);
    assert.ok(op.tags?.length === 1 && tags.has(op.tags[0]), `${key}: one known tag`);
    assert.ok(op.responses?.["200"], `${key}: its answer`);
    if (op.security?.length === 0) {
      assert.equal(op["x-roles"], undefined, `${key}: public, no roles`);
      continue;
    }
    const roles = op["x-roles"];
    assert.ok(Array.isArray(roles) && roles.includes("owner"), `${key}: the owner may call it`);
    for (const r of roles) assert.ok(ROLES.includes(r), `${key}: role ${r}`);
    assert.ok(op.responses["401"] && op.responses["403"], `${key}: says 401 and 403`);
  }
});

test("every reference resolves", () => {
  const walk = (node, path) => {
    if (Array.isArray(node)) return node.forEach((n, i) => walk(n, `${path}/${i}`));
    if (!node || typeof node !== "object") return;
    if (typeof node.$ref === "string") {
      const target = node.$ref.replace(/^#\//, "").split("/").reduce((o, k) => o?.[k], spec);
      assert.ok(target, `${path}: ${node.$ref}`);
    }
    for (const [k, v] of Object.entries(node)) walk(v, `${path}/${k}`);
  };
  walk(spec, "#");
});
