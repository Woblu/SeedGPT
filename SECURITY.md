# Security

## Reporting a problem

Please report security problems privately, through
[GitHub's private vulnerability reporting](https://github.com/Woblu/Seedsmith/security/advisories/new),
rather than in a public issue. You should get a reply within a week.

The latest release is the only supported version.

## What Seedsmith does on your computer

- It runs a small web server that only your own computer can reach (it binds to
  `127.0.0.1`), and it refuses requests from other websites open in your browser.
- Searches run locally. Nothing is uploaded, and there is no telemetry and no
  update check.
- If you add an AI key, the text you type in the prompt box goes to the provider
  you chose, along with a description of the query format, so that it can write
  the search. The key is kept in your browser and is only sent to that provider.
- It writes temporary files to your system's temp folder and removes them when it
  stops. It does not write to its own folder.

## Unsigned downloads

The Windows and macOS downloads are not code-signed, so both systems warn before
running them. You can check a download against `SHA256SUMS.txt` on the release
page, or build from source.
