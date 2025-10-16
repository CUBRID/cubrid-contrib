# Neovim Code Autoformat

## Using Conform.nvim

### Steps

1. Install conform.nvim

`conform.nvim` is a Neovim plugin that provides code formatting on save.

We need to make ti work with CUBRID's source code formatter `cubrid-format-codestyle.sh`.

<https://github.com/stevearc/conform.nvim>

2. Add `cubrid-format-codestyle.sh` to PATH

```bash
# for example,
cp cubrid-format-codestyle.sh /usr/local/bin/
```

> IMPORTANT: cubrid-format-codestyle.sh is dependent on `indent <= 2.2.11` and `astyle`.

2. Copy example.nvim.lua to cubrid source directory.

```bash
git clone https://github.com/CUBRID/cubrid && cd cubrid
cp <path to example.nvim.lua> && ./.nvim.lua
```

`.nvim.lua` is automatically loaded whenever you open neovim inside the cubrid source directory.

`example.nvim.lua` contains a working example for setting up CUBRID's source code formatter with conform.nvim.

In theory, simply copying and renaming `example.nvim.lua` should be enough to get it working.

Once you allow `.nvim.lua` to be executed, `conform.nvim` will automatically format the code on save.
