"""Language-neutral HTTP/JSON interface (standard library only).

  POST /statement  {"cypher": "...", "parameters": {...}?, "parameter_types": {...}?, "index_id": "..."?}
                   -> Result. One entry for everything: a query is rewritten,
                   FILL registers an index (NeuG semantics), DROP_INDEX T(c)
                   removes one, other statements are returned for the target.
  POST /script     {"script": "stmt; stmt; ..."}         -> {"results": [Result, ...]}
  GET  /indexes                                           -> registered indexes
  GET  /schema                                            -> middleware schema (YAML text)
  GET  /build                                             -> target statements materializing the indexes
  GET  /health

With an index registry (--registry):
  GET  /registry          entries, build status and the last report
  POST /registry/build    retry pending builds
Without one (indexes kept in memory only):
  POST /index      {"id", "fills": [...]}  ;  DELETE /index/<id>
"""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote


def make_handler(middleware):
    registry = middleware.registry

    class Handler(BaseHTTPRequestHandler):
        def _send(self, status, payload):
            body = json.dumps(payload, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _json(self):
            length = int(self.headers.get('Content-Length') or 0)
            return json.loads(self.rfile.read(length) or b'{}')

        def log_message(self, fmt, *args):  # quiet by default
            pass

        def do_GET(self):
            if self.path == '/health':
                return self._send(200, {'ok': True, 'dialect': middleware.profile.name})
            if self.path == '/indexes':
                return self._send(200, middleware.indexes())
            if self.path == '/schema':
                return self._send(200, {'schema': middleware.schema.to_yaml()})
            if self.path == '/build':
                return self._send(200, {'indexes': middleware.build_statements()})
            if self.path == '/registry' and registry is not None:
                return self._send(200, dict(registry.describe(), last_report=registry.last_report))
            self._send(404, {'error': 'not found'})

        def do_POST(self):
            from .registry import RegistryError
            try:
                body = self._json()
                if self.path == '/statement':
                    result = middleware.execute(body['cypher'], body.get('parameters'),
                                                body.get('parameter_types'), body.get('index_id'))
                    return self._send(200, result.to_dict())
                if self.path == '/script':
                    return self._send(200, {'results': [r.to_dict() for r in middleware.execute_script(body['script'])]})
                if self.path == '/registry/build' and registry is not None:
                    return self._send(200, registry.build())
                if registry is not None and self.path.startswith('/index'):
                    raise ValueError('Index registry: send FILL / DROP_INDEX to /statement')
                if self.path == '/index/neo4j':
                    results = middleware.register_neo4j_index(body['script'], body.get('id'), body.get('parameters'))
                    return self._send(200, {'results': [r.to_dict() for r in results]})
                if self.path == '/index':
                    result = middleware.register_index(body['fills'], body.get('id'),
                                                       input_dialect=body.get('input_dialect'))
                    return self._send(200, result.to_dict())
                self._send(404, {'error': 'not found'})
            except RegistryError as error:
                self._send(400, {'ok': False, **error.to_dict(), 'report': error.report})
            except Exception as error:  # report, never crash the server
                self._send(400, {'ok': False, 'error': f'{type(error).__name__}: {error}'})

        def do_DELETE(self):
            if self.path.startswith('/index/') and registry is None:
                return self._send(200, {'dropped': middleware.drop_index(unquote(self.path[len('/index/'):]))})
            if self.path.startswith('/index/'):
                return self._send(400, {'error': 'Index registry: send DROP_INDEX TABLE(columns) to /statement'})
            self._send(404, {'error': 'not found'})

    return Handler


def serve(middleware, host='127.0.0.1', port=8765):
    server = ThreadingHTTPServer((host, port), make_handler(middleware))
    try:
        server.serve_forever()
    finally:
        server.server_close()
