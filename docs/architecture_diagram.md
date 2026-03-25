# Architecture Diagram

```mermaid
flowchart LR
    A[Client Apps\nTCP / HTTP / UI] --> B[TCP Server + HTTP Server]
    B --> C[Connection Handler\nPer-client handling]
    C --> D[Command Parser\nSET/GET/DEL/STATS/PING]
    D --> E[Sharded Cache Core\nshared_mutex per shard]

    E --> F[LRU Engine\nDoubly-linked list]
    E --> G[TTL Engine\nExpiry checks + reaper]
    E --> H[Request Collapsing\nIn-flight future map]
    E --> I[Memory Layer\nunordered_map + object sizer]

    E --> J[Stats\nHit/Miss, active keys, bytes]
    J --> K[Dashboard/API Output]
```

## Notes

- Fine-grained concurrency is achieved via sharding and per-shard reader-writer locking.
- LRU and TTL run as cache logic, while storage remains hash-map/list backed.
- Request collapsing ensures only one in-flight compute for hot-expired keys.
