# Secure container deployment

Algorithm Trainer uses a hybrid container deployment on native Linux:

- PostgreSQL runs in an official, version-pinned container with a persistent named volume.
- The compiled React application runs as an unprivileged, read-only Nginx container with all Linux
  capabilities removed.
- The C++ API and judge run directly under a delegated systemd user scope on the host.

Keeping the backend on the host is intentional. NsJail needs nested namespaces and writable delegated
cgroup v2 controllers. Giving an ordinary Docker container enough access to provide those features
requires broad privileges over the host. The launcher never uses `--privileged`, never mounts the
Docker socket, and never falls back to unrestricted execution.

## Host requirements

- Native x86_64 Linux with cgroup v2, unprivileged namespaces, seccomp, and a systemd user session
- Nix with flakes enabled
- Docker Engine with the Compose v2 plugin

Docker Desktop on macOS or Windows is not supported for judging hostile submissions. Those products
run Linux in a virtual machine whose namespace and cgroup delegation differ from the audited native
deployment. Use a dedicated Linux machine or Linux VM and run the launcher inside that machine.

## Lifecycle

From the repository root:

```bash
./scripts/container-app start
./scripts/container-app status
./scripts/container-app logs
./scripts/container-app restart
./scripts/container-app stop
```

`start` builds the backend reproducibly with Nix, builds the frontend image, creates the PostgreSQL
container, applies database migrations, and starts the backend in a delegated systemd scope. Open
<http://127.0.0.1:5173> after it reports ready.

The first start generates `data/container.env` with mode 600. It contains the database password and
initial administrator credentials. Do not commit or copy this file. The PostgreSQL port is published
only on `127.0.0.1`, and the backend also listens only on loopback. The frontend uses host networking
so its reverse proxy can reach that loopback-only API without exposing port 8080.

`stop` removes the containers and network but preserves the `algorithm-trainer_postgresql-data`
volume. To inspect logs:

```bash
tail -f data/container-backend.log
docker compose --env-file data/container.env logs -f
```

## Backup and removal

Back up PostgreSQL before upgrades or destructive maintenance:

```bash
docker compose --env-file data/container.env exec -T database \
  pg_dump -U algorithm_trainer -d algorithm_trainer -Fc > algorithm-trainer.dump
```

Normal `stop` is non-destructive. Removing the named PostgreSQL volume permanently deletes the
container deployment's application data and is therefore deliberately not part of the launcher.

## Production notes

The provided workflow is intended for one trusted Linux host. For an internet-facing instance, put a
TLS reverse proxy in front of port 5173, set `ALGORITHM_TRAINER_SECURE_COOKIES=1` in the launcher
environment, restrict inbound firewall rules, rotate generated credentials, monitor judge failures,
and follow the systemd and sandbox requirements in [`sandbox.md`](sandbox.md).
