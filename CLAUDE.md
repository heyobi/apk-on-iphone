# Working on this repo

- **Work on `main` only.** Commit and push straight to `main`; do not create
  `claude/*` or other side branches. Two sessions once worked in parallel on
  separate branches and duplicated a day of work. If a session is started on a
  side branch, switch to `main` (`git fetch origin main && git checkout main`)
  before changing anything, and pull before you start.
- **Start from `docs/STATUS.md`** (what runs, what is next) and
  `docs/RESEARCH.md` (route and decisions). Update STATUS.md in the same commit
  as the work it describes.
- `make test` must stay green. For a CPU change also run
  `make difftest PYTHON=<python with unicorn>`; every class it runs must show
  `WRONG 0`.
- Never commit Android or app binaries (APKs, `.so`, system images). They are
  produced locally (`tools/android-root.sh`, `tools/apkscan.py`) in scratch space.
- A push to `main` builds the iOS app and publishes `ApkOnIphone.ipa` as a
  GitHub Release (`.github/workflows/ios.yml`). Bump `ios/Info.plist`'s
  version when the app's behaviour changes.
