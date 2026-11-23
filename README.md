# asciirc

A terminal Rocket.Chat client in the style of irssi.

## Build

Needs a C99 compiler and OpenSSL.

```
make
```

## Configure

Create a personal access token in Rocket.Chat (My Account, Personal Access Tokens) and put it into
`~/.config/asciirc/config`:

```
server = https://chat.example.com
user_id = YOUR_USER_ID
token = YOUR_TOKEN
theme = solarized-dark
```

Run `chmod 600 ~/.config/asciirc/config` afterwards otherwise it will complain `;)`

## Run

```
./asciirc
```

Type `/help` inside the client for commands. `./asciirc -h` lists the command line options.

## Usage

Windows:

- `Ctrl+G` go to a window: type a number or part of a name, `Tab` cycles matches, `Enter` switches
- `Ctrl+X` jump to the next window with unread messages
- `Ctrl+N` / `Ctrl+P` next / previous window
- `PgUp` / `PgDn` scroll

Messages:

- `Ctrl+R` select a message, `Up` / `Down` move the selection
- `r` or `Enter` reply in its thread, `Esc` cancels the reply
- `+` react: type an emoji name, `Tab` cycles matches, `Enter` sends, the same emoji again removes it
- `Tab` completes @mentions

Commands: `/join #channel`, `/query user`, `/part`, `/win list`, `/theme name`, `/quit`.
