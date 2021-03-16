# THE YAK ROOM

- It can be built by navigating to src/ and running make
- To run, navigate to src/ and use ./chat_server
- To specify a port, use ./chat_server port_num
	- port_num must be between 49512 & 65535
	- if not port_num specified, 1234 is used
- There is some logic to handle \r for testing with telnet
	- hopefully shouldn't affect normal operation
- It is assumed that clients send their first message in a timely manner after connecting
