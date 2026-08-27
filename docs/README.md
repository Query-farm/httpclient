<img src="https://github.com/user-attachments/assets/46a5c546-7e9b-42c7-87f4-bc8defe674e0" width=250 />

# DuckDB HTTP Client Extension
This very experimental extension spawns an HTTP Client from within DuckDB resolving GET/POST requests.<br>

> Experimental: USE AT YOUR OWN RISK!

### 📦 Installation
```sql
INSTALL http_client FROM community;
LOAD http_client;
```

### Functions
- `http_head(url)`
- `http_get(url)` / `http_get(url, headers, params)`
  - JSON object with `status`, `reason`, `body`, and `headers` (response headers as a JSON object, keys lowercased). Non-UTF-8 bodies are base64-encoded and marked with `"body_base64": true`
- `http_get_blob(url)` / `http_get_blob(url, headers, params)`
  - Typed struct `{status INTEGER, reason VARCHAR, headers MAP(VARCHAR, VARCHAR), body BLOB}` for binary responses (images, protobuf, …)
- `http_post(url, headers, params)`
  - JSON body (`Content-Type: application/json`). `params` may be a **JSON** value (objects, arrays, nested fields) or a **MAP** of strings (JSON-encoded as an object of string values). MAP is the legacy form used in older examples; JSON is required for nested payloads
- `http_post(url, body)` / `http_post(url, headers, body)`
  - Sends POST with a raw `VARCHAR` body (plain text, WarpScript, XML, …). Defaults to `text/plain`; set a `Content-Type` header to override
- `http_post_form(url, headers, params)`
  - Sends POST with `application/x-www-form-urlencoded` encoding
- `http_post_multipart(url, headers, fields, files)`
  - Sends `multipart/form-data` with form fields and in-memory file parts (`BLOB` content, optional filename and content type)

All request functions are **VOLATILE** so a nested call such as `http_post(...)->>'access_token'` is executed once.

Response headers are optional to consume: they are always attached (`res->'headers'->>'link'` or `http_get_blob(url).headers['link']`) and can be ignored.

Authentication uses DuckDB secrets of `TYPE http` (same as httpfs). A matching secret is applied by URL `SCOPE`: `BEARER_TOKEN` becomes an `Authorization` header, and `EXTRA_HTTP_HEADERS` are merged in. Explicit headers on the call win over secret values.

HTTP proxies follow DuckDB’s `http_proxy` / `http_proxy_username` / `http_proxy_password` settings (and the `HTTP_PROXY` environment variable). A `TYPE http` secret may also set `HTTP_PROXY` (plus username/password); the secret wins over session settings.

Retries are off by default. Optional session settings:

```sql
SET http_client_retries = 3;            -- extra attempts (default 0)
SET http_client_retry_wait_ms = 100;    -- base wait before the first retry
SET http_client_retry_backoff = 4.0;    -- exponential multiplier
```

Transport failures are always retried. HTTP 408, 429, 502, 503, and 504 are retried for every method; 500 is retried for GET/HEAD only. 401 and 403 are never retried. A `Retry-After` header on 429 is honored when it is a delay in seconds.

### Concurrency

HTTP functions are ordinary DuckDB scalars. A query over millions of rows does **not** fire millions of requests at once.

- Each DuckDB worker sends requests **one row after another** inside its current vector (up to 2048 rows).
- Several workers may run at once (`SET threads = N`), so the number of in-flight HTTP calls is about **N**, not the table size.
- There is no extra queue or rate limiter beyond DuckDB’s pipeline.

To cap in-flight calls (for example to avoid bursting an API):

```sql
SET http_client_max_parallel = 4;   -- 0 = unlimited (default)
-- optional: also lower DuckDB parallelism
SET threads = 4;
```

`http_client_max_parallel = 1` serializes HTTP across all workers. Combined with `http_client_retries`, a retry holds its slot so other rows wait instead of stampeding the same endpoint.

### Examples
#### GET
```sql
D WITH __input AS (
    SELECT
      http_get(
          'https://httpbin.org/delay/0'
      ) AS res
  ),
  __response AS (
    SELECT
      (res->>'status')::INT AS status,
      (res->>'reason') AS reason,
      unnest( from_json(((res->>'body')::JSON)->'headers', '{"Host": "VARCHAR"}') ) AS features
    FROM
      __input
  )
  SELECT
    __response.status,
    __response.reason,
    __response.Host AS host,
  FROM
    __response
  ;
┌────────┬─────────┬─────────────┐
│ status │ reason  │    host     │
│ int32  │ varchar │   varchar   │
├────────┼─────────┼─────────────┤
│    200 │ OK      │ httpbin.org │
└────────┴─────────┴─────────────┘
```

#### Response headers
Paging APIs such as GitHub expose a `Link` header. Headers are a JSON object on the existing response (keys are lowercased):

```sql
SELECT http_get('https://httpbin.org/get')->'headers'->>'content-type';

SELECT http_get('https://api.github.com/repos/duckdb/duckdb/issues?per_page=1')
       ->'headers'->>'link';
```

#### Binary body (images, files)
`http_get` cannot store invalid UTF-8 in JSON; binary payloads are base64-encoded there. Prefer `http_get_blob` for raw bytes:

```sql
SELECT
  r.status,
  r.headers['content-type'] AS content_type,
  r.body
FROM (SELECT http_get_blob('https://httpbin.org/image/png') AS r);
```

#### POST params: MAP vs JSON vs VARCHAR

`http_post` has three body forms. They are distinct overloads, not implicit casts of each other:

```sql
-- MAP of strings → JSON object {"limit":"10"}  (legacy examples)
SELECT http_post(
    'https://httpbin.org/post',
    headers => MAP { 'accept': 'application/json' },
    params => MAP { 'limit': '10' }
);

-- JSON value → nested objects/arrays as-is
SELECT http_post(
    'https://httpbin.org/post',
    headers => MAP { 'accept': 'application/json' },
    params => {
      'collections': ['sentinel-s2-l2a-cogs'],
      'limit': 10
    }
);

-- VARCHAR → raw body (not JSON-encoded)
SELECT http_post('https://httpbin.org/post', '2 2 +');
```

Form-urlencoded bodies stay on `http_post_form`; file uploads stay on `http_post_multipart`.

#### POST
```sql
D WITH __input AS (
    SELECT
      http_post(
          'https://httpbin.org/delay/0',
          headers => MAP {
            'accept': 'application/json',
          },
          params => MAP {
          }
      ) AS res
  ),
  __response AS (
    SELECT
      (res->>'status')::INT AS status,
      (res->>'reason') AS reason,
      unnest( from_json(((res->>'body')::JSON)->'headers', '{"Host": "VARCHAR"}') ) AS features
    FROM
      __input
  )
  SELECT
    __response.status,
    __response.reason,
    __response.Host AS host,
  FROM
    __response
  ;
┌────────┬─────────┬─────────────┐
│ status │ reason  │    host     │
│ int32  │ varchar │   varchar   │
├────────┼─────────┼─────────────┤
│    200 │ OK      │ httpbin.org │
└────────┴─────────┴─────────────┘
```

#### POST using form encoding(application/x-www-form-urlencoded, not multipart/form-data)
```sql
D WITH __input AS (
  SELECT
    http_post_form(
        'https://httpbin.org/delay/0',
        headers => MAP {
          'accept': 'application/json',
        },
        params => MAP {
          'limit': 10
        }
    ) AS res
),
__response AS (
  SELECT
    (res->>'status')::INT AS status,
    (res->>'reason') AS reason,
    unnest( from_json(((res->>'body')::JSON)->'form', '{"limit": "VARCHAR"}') ) AS features
  FROM
    __input
)
SELECT
  __response.status,
  __response.reason,
  __response.limit AS limit
FROM
  __response
;
┌────────┬─────────┬─────────┐
│ status │ reason  │  limit  │
│ int32  │ varchar │ varchar │
├────────┼─────────┼─────────┤
│    200 │ OK      │ 10      │
└────────┴─────────┴─────────┘
```

#### POST a raw text body
Use this when the API expects plain text rather than a JSON object (for example [Warp 10 WarpScript](https://github.com/Query-farm/httpclient/issues/27)).

```sql
SELECT http_post(
    'https://httpbin.org/post',
    '2 2 +'
)->>'status';

SELECT http_post(
    'https://sandbox.senx.io/api/v0/exec',
    headers => MAP {
      'Content-Type': 'text/plain'
    },
    params => 'REV'
);
```

#### POST multipart form data (fields + in-memory files)

```sql
SELECT http_post_multipart(
    'https://httpbin.org/post',
    headers => MAP {
      'accept': 'application/json'
    },
    fields => MAP {
      'foo': 'bar'
    },
    files => [
      {
        'name': 'data',
        'content': 'col1,col2\n1,2'::BLOB,
        'filename': 'input.csv',
        'content_type': 'text/csv'
      }
    ]
) AS res;
```

#### Proxies

```sql
SET http_proxy = 'localhost:8080';
SET http_proxy_username = 'user';
SET http_proxy_password = 'pass';

CREATE SECRET via_proxy (
    TYPE HTTP,
    SCOPE 'https://httpbin.org',
    HTTP_PROXY 'localhost:8080',
    HTTP_PROXY_USERNAME 'user',
    HTTP_PROXY_PASSWORD 'pass'
);
```

#### Authenticate with a DuckDB secret

```sql
CREATE SECRET api_auth (
    TYPE HTTP,
    SCOPE 'https://httpbin.org',
    BEARER_TOKEN 'my-token'
);

-- Authorization: Bearer my-token is attached automatically
SELECT http_get('https://httpbin.org/bearer');

CREATE OR REPLACE SECRET api_auth (
    TYPE HTTP,
    SCOPE 'https://httpbin.org',
    EXTRA_HTTP_HEADERS MAP {
      'X-Api-Key': 'super-secret'
    }
);
```

#### Full Example w/ spatial data
This is the original example by @ahuarte47 inspiring this community extension.

```sql
D SET autoinstall_known_extensions=1; SET autoload_known_extensions=1;
D LOAD json; LOAD httpfs; LOAD spatial;

D WITH __input AS (
    SELECT
      http_get(
        'https://earth-search.aws.element84.com/v0/search')
      AS res
  ),
  __features AS (
    SELECT
      unnest( from_json(((res->>'body')::JSON)->'features', '["json"]') )
      AS features
    FROM
      __input
  )
  SELECT
    features->>'id' AS id,
    features->'properties'->>'sentinel:product_id' AS product_id,
    concat(
      'T',
      features->'properties'->>'sentinel:utm_zone',
      features->'properties'->>'sentinel:latitude_band',
      features->'properties'->>'sentinel:grid_square'
    ) AS grid_id,
    ST_GeomFromGeoJSON(features->'geometry') AS geom
  FROM
    __features
  ;
┌──────────────────────┬──────────────────────┬─────────┬──────────────────────────────────────────────────────────────────────────────────┐
│          id          │      product_id      │ grid_id │                                       geom                                       │
│       varchar        │       varchar        │ varchar │                                     geometry                                     │
├──────────────────────┼──────────────────────┼─────────┼──────────────────────────────────────────────────────────────────────────────────┤
│ S2B_55GDP_20241003…  │ S2B_MSIL2A_2024100…  │ T55GDP  │ POLYGON ((146.7963024570636 -42.53859799130381, 145.7818492341335 -42.53284395…  │
│ S2B_55HEC_20241003…  │ S2B_MSIL2A_2024100…  │ T55HEC  │ POLYGON ((146.9997932100229 -34.429312828654396, 146.9997955899612 -33.4390429…  │
│ S2B_55JHN_20241003…  │ S2B_MSIL2A_2024100…  │ T55JHN  │ POLYGON ((149.9810192714723 -25.374826158099584, 149.9573295859729 -24.3845516…  │
│ S2B_15MWT_20230506…  │ S2B_MSIL2A_2023050…  │ T15MWT  │ POLYGON ((-92.01266261624052 -2.357695714729873, -92.0560908879947 -2.35076658…  │
│ S2B_16PBT_20230506…  │ S2B_MSIL2A_2023050…  │ T16PBT  │ POLYGON ((-88.74518736203468 11.690012668805194, -88.9516536515512 11.72635252…  │
│ S2B_16PCT_20230506…  │ S2B_MSIL2A_2023050…  │ T16PCT  │ POLYGON ((-87.82703591176752 11.483638069337541, -88.8349824533826 11.70734355…  │
│ S2B_15PZP_20230506…  │ S2B_MSIL2A_2023050…  │ T15PZP  │ POLYGON ((-89.24113885498912 11.784951995968179, -89.38831685490888 11.8080246…  │
│ S2B_16PET_20230506…  │ S2B_MSIL2A_2023050…  │ T16PET  │ POLYGON ((-87.00017408768262 11.277451946475995, -87.00017438483464 11.7600349…  │
│ S2B_16PBU_20230506…  │ S2B_MSIL2A_2023050…  │ T16PBU  │ POLYGON ((-88.74518962519173 11.690373971442378, -89.62017907866615 11.8466519…  │
│ S2B_16PDU_20230506…  │ S2B_MSIL2A_2023050…  │ T16PDU  │ POLYGON ((-87.91783982214183 11.670141095427311, -87.92096676562824 12.5828090…  │
├──────────────────────┴──────────────────────┴─────────┴──────────────────────────────────────────────────────────────────────────────────┤
│ 10 rows                                                                                                                        4 columns │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```
