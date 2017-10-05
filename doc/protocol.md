## Protocol

Both POST and GET messages are exchanged over a ZeroMQ Request-Reply socket using a CoAP-like message format. A message contains a header followed by an optional token, any message options then an optional payload. For example, a POST message must contain a header, options and a payload, whereas a GET message will not contain a payload. An acknowledgement may consist only of a header but can also consist of a header, options, and payload.

An OBSERVE message is slightly more complex and involves an initial Request-Reply exchange to setup communication over a Broker-Dealer socket. This allows multiple clients to connect to the server and receive updates posted to an observed path. An OBSERVE message is implemented as a special type of GET message with an observe option set. The server replies to the GET with a UUID in the payload which is used to identify the client over the Broker-Dealer communication.

### message structure

All values are in bits unless specified.

#### header
| version  | tkl | oc | code |
| :--: | :--: |  :--: | :--: |
| 4 | 16 (network order) | 4 | 8 |

* tlk = token length in bytes
* oc = number of options present
* code = CoAP specified

#### token (optional)
| token |
| :--: |
| bytes |
#### options (repeating)
| number  | length | pad | value | ... | 
| :--: | :--: |  :--: | :--: | :--: | 
| 4 | 16 (network order) | 4 | bytes | ... |

* number = CoAP specified
* value = CoAP specified

#### message payload (optional)
| payload |
| :--: |
| bytes |