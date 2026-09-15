# STEP test corpus

The unit tests generate their own fixtures (`tests/fixtures.cpp`), so nothing here is required for `ctest`.

`fetch.py` downloads public-domain / permissively licensed STEP files into this directory and `run.py` imports
every file through `opad-cli`, recording success, body counts, healing and timing in `corpus-report.json`. The
robustness target is "opens 95% of a 200-file public corpus without error"; add sources to `SOURCES` in
`fetch.py` as they are found. Downloaded files are git-ignored.

```
python tests/corpus/fetch.py            # download
python tests/corpus/run.py build/bin/opad-cli   # import them all, print the report
```
