# Dionisio

Snack and beverage manager for a small office. Employees set preferences in Slack with `/dionisio`; the admin imports supplier orders (JUNTOS and BEES JSON exports), ticks off what arrived, and gets Slack DM alerts for each order's Boleto. A C++20 server runs on Cloud Run (free tier), and a local terminal UI (TUI) talks to it over `/api`.

Orders run in 14-day cycles anchored on Monday `2026-11-09` (`CYCLE_ANCHOR`). Preferences expire at the end of each cycle. Money is integer centavos, shown as `R$ 895.16`. Dates use a fixed UTC-3 clock.

## Build and run locally

Needs a C++20 compiler, CMake, libcurl, SQLite3 and OpenSSL (server only). On macOS: `brew install openssl@3`.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
cmake --build build -j
ctest --test-dir build --output-on-failure

API_TOKEN=dev SLACK_SIGNING_SECRET=dev CYCLE_ANCHOR=2026-11-09 ADMIN_SLACK_IDS=UADMIN \
  DB_PATH=./dev.db PORT=8080 ./build/dionisio-server
```

Server-only build (what the Dockerfile does): `-DDIONISIO_TUI=OFF`.

## Server environment

| Variable | Notes |
|---|---|
| `PORT` | HTTP port |
| `DB_PATH` | Default `/tmp/dionisio.db` |
| `API_TOKEN` | Bearer token for `/api/*` and `/jobs/daily` |
| `SLACK_SIGNING_SECRET` | Verifies `/slack/*` requests |
| `SLACK_BOT_TOKEN` | Empty means dry-run: DMs are logged, not sent |
| `SLACK_API_BASE` | Default `https://slack.com/api` |
| `ADMIN_SLACK_IDS` | Comma-separated Slack user IDs that get Boleto alerts. Required when `GCS_BUCKET` is set |
| `CYCLE_ANCHOR` | `2026-11-09` |
| `GCS_BUCKET` | Empty means local file only |
| `GCS_ENDPOINT` | Default `https://storage.googleapis.com`; if set, the metadata token is skipped (tests) |
| `ALLOW_TODAY_OVERRIDE` | `1` honours `?today=` (tests only) |

Persistence: with `GCS_BUCKET` set, the SQLite file is restored from one GCS object at startup and uploaded after every mutating request, with `ifGenerationMatch`. If another writer got there first (deploy overlap), the server reloads the latest copy and answers 503, and the client retries. Each upload is tried once, with no 429 retry (a deviation from the plan): a failed upload answers 503, and the next mutating request, even the client's no-op retry, uploads the pending change. If the restore fails for any reason other than "not found", the server exits instead of starting empty.

## TUI

```bash
export DIONISIO_URL=https://YOUR-RUN-URL DIONISIO_TOKEN=<API_TOKEN>
./build/dionisio import tests/fixtures/bees-pedido-9354967539.json   # import an order export
./build/dionisio                                                      # open the UI
```

Tabs: Home (suggestions, orders, Boletos), Order checklist (Enter = arrived, Backspace/Delete = missing, `p` = pending, `s` or leaving the screen saves), Catalogue (pack size, minimum, active), New items, Past cycles.

## Slack app

1. Deploy first so you have the Cloud Run URL.
2. https://api.slack.com/apps -> Create New App -> From manifest -> paste `slack-manifest.yml` after replacing `YOUR-RUN-URL` with the Cloud Run host.
3. Install to the workspace. Put the Signing Secret and the Bot User OAuth Token (`xoxb-...`) in the secrets below.

Scopes: `commands`, `chat:write`, `im:write`. The Messages tab is enabled read-only so people can see the bot's DMs.

## One-time GCP setup

Set once: `PROJECT_ID`, `PROJECT_NUMBER`, `BUCKET=$PROJECT_ID-dionisio`. Region is `us-central1` everywhere.

```bash
# 1. APIs
gcloud services enable run.googleapis.com artifactregistry.googleapis.com secretmanager.googleapis.com \
  cloudscheduler.googleapis.com iamcredentials.googleapis.com --project $PROJECT_ID

# 2. Bucket: versioning + keep 20 noncurrent versions
gcloud storage buckets create gs://$BUCKET --location us-central1 --project $PROJECT_ID
gcloud storage buckets update gs://$BUCKET --versioning
echo '{"rule":[{"action":{"type":"Delete"},"condition":{"numNewerVersions":20,"isLive":false}}]}' > lifecycle.json
gcloud storage buckets update gs://$BUCKET --lifecycle-file=lifecycle.json

# 3. Artifact Registry, keep 3 images
gcloud artifacts repositories create dionisio --repository-format docker --location us-central1 --project $PROJECT_ID
echo '[{"name":"keep-3","action":{"type":"Keep"},"mostRecentVersions":{"keepCount":3}}]' > cleanup.json
gcloud artifacts repositories set-cleanup-policies dionisio --location us-central1 --policy cleanup.json --no-dry-run --project $PROJECT_ID

# 4. Secrets. The API token is random; keep $API_TOKEN for the TUI and the Scheduler job (step 10).
API_TOKEN=$(openssl rand -hex 32)
for s in api-token slack-signing-secret slack-bot-token; do
  gcloud secrets create $s --replication-policy automatic --project $PROJECT_ID
done
printf '%s' "$API_TOKEN" | gcloud secrets versions add api-token --data-file=- --project $PROJECT_ID
printf '%s' "<Slack signing secret>" | gcloud secrets versions add slack-signing-secret --data-file=- --project $PROJECT_ID
printf '%s' "<xoxb-... bot token>" | gcloud secrets versions add slack-bot-token --data-file=- --project $PROJECT_ID

# 5. Runtime service account
gcloud iam service-accounts create dionisio-run --project $PROJECT_ID
RUN_SA=dionisio-run@$PROJECT_ID.iam.gserviceaccount.com
gcloud storage buckets add-iam-policy-binding gs://$BUCKET --member serviceAccount:$RUN_SA --role roles/storage.objectUser
for s in api-token slack-signing-secret slack-bot-token; do
  gcloud secrets add-iam-policy-binding $s --member serviceAccount:$RUN_SA --role roles/secretmanager.secretAccessor --project $PROJECT_ID
done

# 6. Deploy service account
gcloud iam service-accounts create dionisio-deploy --project $PROJECT_ID
DEPLOY_SA=dionisio-deploy@$PROJECT_ID.iam.gserviceaccount.com
for r in roles/run.developer roles/artifactregistry.writer; do
  gcloud projects add-iam-policy-binding $PROJECT_ID --member serviceAccount:$DEPLOY_SA --role $r
done
gcloud iam service-accounts add-iam-policy-binding $RUN_SA --member serviceAccount:$DEPLOY_SA --role roles/iam.serviceAccountUser

# 7. Workload Identity Federation (no keys)
gcloud iam workload-identity-pools create github --location global --project $PROJECT_ID
gcloud iam workload-identity-pools providers create-oidc github --location global \
  --workload-identity-pool github --project $PROJECT_ID \
  --issuer-uri https://token.actions.githubusercontent.com \
  --attribute-mapping google.subject=assertion.sub,attribute.repository=assertion.repository,attribute.ref=assertion.ref \
  --attribute-condition "assertion.repository=='Thinato/dionisio' && assertion.ref=='refs/heads/main'"
gcloud iam service-accounts add-iam-policy-binding $DEPLOY_SA --role roles/iam.workloadIdentityUser \
  --member "principalSet://iam.googleapis.com/projects/$PROJECT_NUMBER/locations/global/workloadIdentityPools/github/attribute.ref/refs/heads/main"
# Only main can deploy: the provider condition and the binding both require refs/heads/main.
# Also turn on branch protection for main (require PR + passing ci) so nobody pushes straight to it.

# 8. GitHub repo variables (Settings -> Secrets and variables -> Actions -> Variables)
gh variable set PROJECT_ID --repo Thinato/dionisio --body $PROJECT_ID
gh variable set WIF_PROVIDER --repo Thinato/dionisio --body projects/$PROJECT_NUMBER/locations/global/workloadIdentityPools/github/providers/github
gh variable set DEPLOY_SA --repo Thinato/dionisio --body $DEPLOY_SA
gh variable set RUN_SA --repo Thinato/dionisio --body $RUN_SA
gh variable set GCS_BUCKET --repo Thinato/dionisio --body $BUCKET
gh variable set ADMIN_SLACK_IDS --repo Thinato/dionisio --body "U01ABC,U02DEF"

# 9. After the first CI deploy: make the service public (Slack must reach it; /api stays bearer-protected)
gcloud run services add-iam-policy-binding dionisio --region us-central1 --member allUsers --role roles/run.invoker

# 10. Daily 09:00 BRT job (if Authorization is refused, use --headers "X-Api-Token=$API_TOKEN")
URL=$(gcloud run services describe dionisio --region us-central1 --format='value(status.url)')
gcloud scheduler jobs create http dionisio-daily --location us-central1 --schedule "0 9 * * *" \
  --time-zone America/Sao_Paulo --uri $URL/jobs/daily --http-method POST \
  --headers "Authorization=Bearer $API_TOKEN" --max-retry-attempts 3 --min-backoff 5m --attempt-deadline 60s
```

Rotating the API token: add a new `api-token` secret version, redeploy (or bump a revision), then update the job's header with `gcloud scheduler jobs update http dionisio-daily --location us-central1 --update-headers "Authorization=Bearer $NEW_TOKEN"` and the TUI's `DIONISIO_TOKEN`.

11. Create the Slack app from the manifest (see above).
12. Set a billing budget alert at R$5.

## Deploy and verify

Merging to `main` runs `.github/workflows/ci.yml`: build + tests, then build the image, deploy to Cloud Run, and smoke-test that `GET /api/home` without a token returns 401.

```bash
curl -s -H "Authorization: Bearer $API_TOKEN" $URL/api/home        # note the order count
gcloud run deploy dionisio --region us-central1 --image "<same image>" --update-env-vars BUMP=$(date +%s)
curl -s -H "Authorization: Bearer $API_TOKEN" $URL/api/home        # same count: restored from GCS
gcloud scheduler jobs run dionisio-daily --location us-central1
```

Cold starts can miss Slack's 3 s window; just run `/dionisio` again.
