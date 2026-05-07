# Test client_quic protocol

## Prepare
1. Kill cfrp processes.
2. compile latest code.
3. Start server with `../server.toml`
4. Start client with `client_quic_protocol_local.toml`

## Expections:
1. when client starts, connection should be establish without delay.
2. when only server exits (ctrl + c or killed), client should fall into retry connection right away.
3. when only client exits, server should log the existing of client and has proper cleaning.
4. server and client should be pacefuly exited by ctrl + c.

## Clean
kill all cfrp processes.

# Test client_tcp protocol

same steps as `Test client_quic protocol` but use `client_tcp_protocl_local.toml`


# Test client_ws protocol

same steps as `Test client_quic protocol` but use `client_ws_protocl_local.toml`

