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
