# CUBRID autoformatter for VS Code

## Installation

1. Install `jkillian.custom-local-formatters`

<https://marketplace.visualstudio.com/items?itemName=jkillian.custom-local-formatters>

2. Set up VS Code settings.json to use `custom-local-formatters` as the default formatter for c & cpp.

```json
"[cpp]": {
    "editor.formatOnSave": true,
    "editor.defaultFormatter": "jkillian.custom-local-formatters"
},
"[c]": {
  "editor.formatOnSave": true,
  "editor.defaultFormatter": "jkillian.custom-local-formatters"
},
```

> TIP: It can be User Settings or Workspace Settings.

3. Set up `custom-local-formatters` to use `cubrid-format-codestyle-stdin.sh` for c & cpp.

```json
  "customLocalFormatters.formatters": [
    {
      "command": "cubrid-format-codestyle-stdin.sh ${file}",
      "languages": ["c", "cpp"]
    }
  ],
```

4. Add `cubrid-format-codestyle-stdin.sh` to `$PATH`.

> TIP: In order for `custom-local-formatters` to work, the formatter command must take codes as stdin and output to stdout.
That is why we need to modify the official CUBRID GitHub Actions codestyle.sh to output to stdout, instead of in-place update.

> TIP 2: Install the appropriate `indent` and `astyle` for the shell script to work.

> IMPORTANT: install indent <= 2.2.11

