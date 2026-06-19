const fs = require("fs");
const https = require("https");
const path = require("path");

const HOST = "0.0.0.0";
const PORT = 7777;
const ROOT = __dirname;
const PUBLIC_DIR = path.join(ROOT, "public");
const CERT_DIR = path.join(ROOT, "certs");
const LATEST_IMAGE = path.join(ROOT, "latest.jpg");
const LATEST_META = path.join(ROOT, "latest.json");

const serverOptions = {
  key: fs.readFileSync(path.join(CERT_DIR, "key.pem")),
  cert: fs.readFileSync(path.join(CERT_DIR, "cert.pem")),
};

let latestStatus = {
  frameCount: 0,
  updatedAt: null,
  camera: null,
  width: null,
  height: null,
  bytes: 0,
};

function send(res, status, body, headers = {}) {
  const payload = typeof body === "string" || Buffer.isBuffer(body)
    ? body
    : JSON.stringify(body);

  res.writeHead(status, {
    "Cache-Control": "no-store",
    ...headers,
  });
  res.end(payload);
}

function serveStatic(req, res) {
  const url = new URL(req.url, `https://${req.headers.host}`);
  const requested = url.pathname === "/" ? "/index.html" : url.pathname;
  const filePath = path.normalize(path.join(PUBLIC_DIR, requested));

  if (!filePath.startsWith(PUBLIC_DIR)) {
    send(res, 403, "Forbidden", { "Content-Type": "text/plain; charset=utf-8" });
    return;
  }

  fs.readFile(filePath, (err, data) => {
    if (err) {
      send(res, 404, "Not found", { "Content-Type": "text/plain; charset=utf-8" });
      return;
    }

    const ext = path.extname(filePath).toLowerCase();
    const contentType = {
      ".html": "text/html; charset=utf-8",
      ".css": "text/css; charset=utf-8",
      ".js": "application/javascript; charset=utf-8",
      ".jpg": "image/jpeg",
      ".png": "image/png",
      ".json": "application/json; charset=utf-8",
    }[ext] || "application/octet-stream";

    send(res, 200, data, { "Content-Type": contentType });
  });
}

function readBody(req, limitBytes = 12 * 1024 * 1024) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;

    req.on("data", (chunk) => {
      total += chunk.length;
      if (total > limitBytes) {
        reject(new Error("Request body too large"));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });

    req.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
    req.on("error", reject);
  });
}

async function handleFrame(req, res) {
  try {
    const raw = await readBody(req);
    const body = JSON.parse(raw);
    const match = /^data:image\/jpeg;base64,(.+)$/.exec(body.image || "");

    if (!match) {
      send(res, 400, { ok: false, error: "Expected JPEG data URL" }, {
        "Content-Type": "application/json; charset=utf-8",
      });
      return;
    }

    const image = Buffer.from(match[1], "base64");
    fs.writeFileSync(LATEST_IMAGE, image);

    latestStatus = {
      frameCount: latestStatus.frameCount + 1,
      updatedAt: new Date().toISOString(),
      camera: body.camera || null,
      width: body.width || null,
      height: body.height || null,
      bytes: image.length,
    };
    fs.writeFileSync(LATEST_META, JSON.stringify(latestStatus, null, 2));

    send(res, 200, { ok: true, ...latestStatus }, {
      "Content-Type": "application/json; charset=utf-8",
    });
  } catch (err) {
    send(res, 500, { ok: false, error: err.message }, {
      "Content-Type": "application/json; charset=utf-8",
    });
  }
}

const server = https.createServer(serverOptions, (req, res) => {
  if (req.method === "POST" && req.url === "/frame") {
    handleFrame(req, res);
    return;
  }

  if (req.method === "GET" && req.url === "/status") {
    send(res, 200, latestStatus, {
      "Content-Type": "application/json; charset=utf-8",
    });
    return;
  }

  if (req.method === "GET" && req.url === "/latest.jpg") {
    if (!fs.existsSync(LATEST_IMAGE)) {
      send(res, 404, "No frame captured yet", { "Content-Type": "text/plain; charset=utf-8" });
      return;
    }
    send(res, 200, fs.readFileSync(LATEST_IMAGE), { "Content-Type": "image/jpeg" });
    return;
  }

  if (req.method === "GET") {
    serveStatic(req, res);
    return;
  }

  send(res, 405, "Method not allowed", { "Content-Type": "text/plain; charset=utf-8" });
});

server.listen(PORT, HOST, () => {
  console.log(`iPhone camera bridge listening on https://${HOST}:${PORT}`);
  console.log(`Latest frame: ${LATEST_IMAGE}`);
});
