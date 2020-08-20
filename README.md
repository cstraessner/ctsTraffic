# ctsTraffic
ctsTraffic is a highly scalable client/server networking tool giving detailed performance and reliability analytics

If you would like to download the latest build and not have to pull down the source code to build it yourself, you can download them from https://github.com/microsoft/ctsTraffic/tree/master/Releases/2.0.2.5 .

---
# A Practical Guide
---

ctsTraffic was a tool initially developed just after Windows 7 shipped
to accurately measure how our diverse network deployments scale, as well 
as assessing its network reliability. Since then we have added a huge number
of options to work within an increasingly growing number of deployments. This
document reviews the 90% case that most people would likely want to start.

# Good-Put #

ctsTraffic is deliberately designed and implemented to demonstrate
various best-practice guidance we (Winsock) have provided app developers
for designing efficient and scalable solutions. It has a "pluggable"
model where we have author multiple different IO models -- but the
default IO model is what will be most scalable for most network-facing
applications.

As our IO models are implemented to model what we want apps and services
to build, the resulting performance data is a strong reflection of what
one can expect normal apps and services to see in the tested deployment.
This throughput measurement of data *as seen from the app* is commonly
referred to as "good-put" (as opposed to "through-put" which is
generally measured at the hardware level in raw bits/sec).

## A suggested starting point: measuring Good Put ##

The below set of options (using most default options) is generally a
good starting point when measuring good put and reliability. These
options will have clients maintain 8 TCP connections with the server,
sending 1GB of data per connection. Data will be flowing
unidirectionally from the client to the server ('upload' scenarios).

These options will also a good starting point to track the reliability
of a network deployment. It provides data across multiple reliability
pivots:

-   Reliably establishing connections over time
-   Reliably in maintaining connections, sending precisely 1GB of data,
    with both sides agreeing on the number sent and received
-   Reliability in data integrity for all bytes received: every buffer
    received is validated against a specific bit-pattern that we use to
    catch data corruption

| **Server**                    | **Client**                                   |
| ----------------------------- | -------------------------------------------- |
| `ctsTraffic.exe `             | `ctsTraffic.exe `                            |
| `-listen:* `                  | `-target:<server> `                          |
| `-consoleverbosity:1 `        | `-consoleverbosity:1 `                       |
|                               | `-statusfilename:clientstatus.csv `          |
|                               | `-connectionfilename:clientconnections.csv ` |

**Note:** if one needs to measure the other direction, the clients
receiving data from servers, one should append **-pattern:pull** to
the above commands on **both the client and the server**.

We found the above default values to generally be an effective balance
when measuring Good Put, balancing the number of connections being
established to send and receive data with the number of bytes being sent
per connection. We found these values scale very well across many
scenarios: down to small devices with slower connections and up to
reaching 10Gbit deployments. (**Note**: once one gets to 10Gb we
recommend doubling the number of connections and moving to 1TB of data
sent; increasing both again at 40Gb).


## Explaining the console output ##

As a sample run, the below is output from a quick test ran over loopback
(client and server were both run on my same machine). Note that the
-consoleverbosity: flag controls the type and detail of what it output
to the console (setting 0 turns off all output).

**C:\\Users\\kehor\\Desktop\\2.0.1.7\> ctsTraffic.exe -target:localhost
 -consoleverbosity:1 -statusfilename:clientstatus.csv
 -connectionfilename:clientconnections.csv**

` Configured Settings`
 
        Protocol: TCP
        Options: InlineIOCP
        IO function: Iocp (WSASend/WSARecv using IOCP)
        IoPattern: Push \<TCP client send/server recv\>
        PrePostRecvs: 1
        PrePostSends: 1
        Level of verification: Connections & Data
        Port: 4444
        Buffer used for each IO request: 65536 \[0x10000\] bytes
        Total transfer per connection: 1073741824 bytes
        Connecting out to addresses:
               [::1]:4444
               127.0.0.1:4444
        Binding to local addresses for outgoing connections:
               0.0.0.0
               ::
       Connection limit (maximum established connections): 8 \[0x8\]
       Connection throttling rate (maximum pended connection attempts): 1000 \[0x3e8\]
       Total outgoing connections before exit (iterations \* concurrent connections) : 0xffffffffffffffff

` Legend:`
      
    * TimeSlice - (seconds) cumulative runtime
    * Send & Recv Rates - bytes/sec that were transferred within the TimeSlice period
    * In-Flight - count of established connections transmitting IO pattern data
    * Completed - cumulative count of successfully completed IO patterns
    * Network Errors - cumulative count of failed IO patterns due to Winsock errors
    * Data Errors - cumulative count of failed IO patterns due to data errors
 
| TimeSlice | SendBps | RecvBps | In-Flight | Completed | NetError | DataError |
| --------: | ------: | ------: | --------: | --------: | -------: | --------: |
| 0.001 | 0  | 0  | 8  | 0 | 0 | 0 |
| 5.002 | 2635357062 | 124  | 8 | 8 | 0 | 0 |
| 10.003 | 2519263596 | 171 | 8 | 19 | 0 | 0 |
| 15.001 | 2437002784 | 202 | 8 | 32 | 0 | 0 |
| 20.002 | 2639655364 | 171 | 8 | 43 | 0 | 0 |
| 25.002 | 2557516185 | 218 | 8 | 57 | 0 | 0 |

` Historic Connection Statistics (all connections over the complete lifetime) `

` SuccessfulConnections [59] NetworkErrors [0] ProtocolErrors [0] `

` Total Bytes Recv : 5194`

` Total Bytes Sent : 67358818304`

` Total Time : 26357 ms.`


### Configured Settings ###
The banner under "Configured Settings" shows many of the defaulted
options.

- Default is to establish TCP connections using OVERLAPPED I/O with IO
    completion ports managed by the NT threadpool, by default handling
    inline successful completions. Thus all send and receive requests
    will loop until an IO request pends, where we will wait for the
    completion port to notify when it completes, when we'll continue to
    send and receive.  
    -   *IO model is configurable: -IO. Inline IO completions is
        configurable: -inlineCompletions. Protocol is configurable:
        -protocol.*
- Default pattern when sending and receiving is to "Push" data,
  directionally sending data from the client to the server.
    -   *The pattern to send and receive is configurable: -pattern.*
- Default is to use 64K buffers for every send and receive request,
  transferring a total of 1GB of data.
    -   *The buffer size used for each IO request is configurable:
        -buffer. The total amount of data to transfer over each TCP
        connection is configurable: -transfer.*
- Shows all resolved addresses which will be used in a round-robin
  fashion as connections are made.
    -   *Target IP addresses is configurable using one or more -Target
        options, specifying a name or IP address servers which to
        connect.*
- Shows that will use ephemeral binding (binding to 'any' address of
  all zeros lets the TCP/IP stack choose the best route to each target
  address).
    -   *The local addresses to use for outbound connections is
        configurable: -bind.*
- Default is to keep 8 concurrent connections established and moving
  data. Will throttle outgoing connections by only keeping up to 1000
  connection attempts in flight at any one time (will above our 8
  concurrent connections ).
    -   *The number of connections to establish is configurable:
        -connections. The connection throttling limit is configurable
        (though not recommended): -throttleConnections.*
- Default is to indefinitely continue making connections as individual
  connections complete -- maintaining 8 connections at all time.
    -   *The total number of connections is configurable: -connections:
        and -iterations. The total connections is the product of
        connections \* iterations.*
    -   *e.g. -connections:100 -iterations:10 will iterate 10 times over
        100 connections for a total of 1000 connections, at which point
        ctsTraffic will gracefully exit.*


### -consoleverbosity:1 ###
Setting console verbosityto 1 will output an aggregate status at each time
slice. The default time slice is every 5 seconds; the time slice is
configurable: -statusUpdate. At every 5 seconds, a line will be output
communicating the following aggregate information:

- *TimeSlice*: the time window in seconds with millisecond precision,
  starting from when ctsTraffic was launched (since -statusUpdate can
  set update frequency in milliseconds)
- *SendBps*: the sent bytes/second [within that specific time
  slice]
- *RecvBps*: the received bytes/second [within that specific time
  slice]
- *Inflight*: the [current number] of active TCP connections
  at this time the slice was recorded
- *Completed*: the [total number] of TCP connections which
  successfully send and receive all bytes at the time the slice was
  recorded
- *NetError*: the [total number] of TCP connections which
  failed due to a Winsock API failing (generally failing to connect,
  send, or receive) at the time the slice was recorded
- *DataError*: the [total number] of TCP connections which
  failed due to data validation errors (received too many bytes,
  received too few bytes before completing the TCP connection,
  discovered data errors when receiving data) at the time the slice
  was recorded

This output serves to give the viewer a quick assessment of what is, and has,
occurred across the TCP connections that were established. The output
functions the same on both the client and the server.


### Options for controlling how long a test runs ###

ctsTraffic has a few options for controlling the amount of time for a
run before it exits.

The manual approach is to just hit CTRL-C in the command-shell.
ctsTraffic recognizes the key-press and will gracefully exit, ensuring
data is accurately flushed to all log files.

The client can control its exit through 2 possible parameter
combinations:

-   **-connections and -iterations**
    -   ctsTraffic semantics to track TCP connections are with the sum
        of these 2 options. If a client needs to eventually go through
        1,000 connections but only keeping 100 connections active at any
        one time, one should say **-connections:100 -iterations:10**.
        After 1,000 total connections, ctsTraffic for that client will
        gracefully exit.
-   **-timeLimit**
    -   ctsTraffic clients can also run for a specified period of time
        based off the **-timeLimit** parameter. The parameter expresses the
        number of milliseconds to run before exiting.

The server can control its exit through just 1 option -- as it is
designed to accept any number of connections from any number of clients.

-   **-serverExitLimit**
    -   ctsTraffic servers can gracefully exit automatically after
        handling the specified number of client connections as set in
        **-serverExitLimit**. For example, if one wanted a server to handle
        10 connections and exit, one would append **-serverExitLimit:10**.
        This ctsTraffic server instance would accept the first 10
        inbound connections and complete the specified IO for each of
        those 10. Any other connections attempted would not be accepted,
        and after completing those 10 connections the server would
        gracefully exit.


### Explaining the generate log files ###

In the same sample as above, two log files were created due to the
following command line options: **-statusfilename:clientstatus.csv
-connectionfilename:clientconnections.csv**. The csv extension informed
ctsTraffic output the files in a comma-separated values format (any
other extension would be written as a line of text).


#### StatusFilename ####

The status file writes out the same information to a csv as is written
to console with the above **-consoleverbosity:1** option set. This is
useful for later analysis, notably in an application like Excel.
Imported into Excel, 25 seconds worth of data would look like this:

 | **TimeSlice** | **SendBps** | **RecvBps** | **In-Flight** | **Completed** | **NetError** | **DataError** |
 | ------------: | ----------: | ----------: | ------------: | ------------: | -----------: | ------------: |
 | 0.001     |      2           |  0       |      8       |        0       |        0      |        0 |
 | 5.002     |      2635514317  |  124     |      8       |        8       |        0      |        0 |
 | 10.003    |      2519781898  |  171     |      8       |        19      |        0      |        0 |
 | 15.001    |      2437029014  |  202     |      8       |        32      |        0      |        0 |
 | 20.002    |      2639838829  |  171     |      8       |        43      |        0      |        0 |
 | 25.002    |      2557568614  |  218     |      8       |        57      |        0      |        0 |


This becomes useful as Excel can quickly give richer views into our
data.

Immediately we can look at the last line at **Completed** (there were 57
successful TCP connections when the last time slice was recorded),
**NetError** and **DataError** (there were 0 failed TCP connections
either through network failures or data errors).

For example, if we wanted to take an average of the **SendBps** values
starting at time slice 5, we would simply specify this in a cell,
`=AVERAGE(B3:B7)`. Similarly we can see the min and max with
`=MIN(B3,B7)` and `=MAX(B3:B7)` respectively. With longer runs and large
data sets, this can be notably powerful to understand the overall
performance metrics of a run.

Additionally, Excel does quick graphing, which can be some of the most
powerful ways to view data. With a larger dataset (with the same above
commands specified above), the final graph for `SendBps` on the client
looked like this:

[\[CHART\]]{.chart}

[\[CHART\]]{.chart}

Bits per second was generated by adding a column and telling Excel to
multiple SendBps \* 8 (the result looks like a consistent \~20 Gbps).


#### ConnectionFilename ####

The status file writes out per connection information to a csv (this
would be the same as what is written to console with
"**-consoleverbosity:3**" option set). This is useful for later analysis
when wanting to look at patterns across a long test run.

Here's an example from the above sample run of the first 10 connections:

![](media/image1.emf){width="6.5in" height="1.3652777777777778in"}

In this log file we can see individual TCP connections recorded.

-   The TCP tuple information (the local address and port, the remote
    address and port)
-   Sent data details (the total \# of sent bytes and the SendBps for
    that one connection)
-   Received data details (the total \# of received bytes and the
    RecvBps for that one connection)
-   The total time required for this connection to complete its transfer
    (TimeMs)
-   The end result showing success, the error code if a networking
    error, or the type of data error if a data error was observed
-   The "ConnectionId" -- a shared GUID from the server and client --
    useful when scenarios need to correlate client logs to server logs
    (even more useful when going through NATs and load balancers where
    addresses don't always provide unique reference points)

As with the Status file analysis, Excel can give deeper insight into the
test run. For example:

-   One could look at failure patterns: did they occur randomly or in
    specific groups of time
-   One could look at which remote addresses had more failures
-   One could look at patterns in SendBps and RecvBps, correlating with
    remote servers to see which servers were more performant
-   One could look at TimeMs to get a broader view over fairness between
    connections, were some connections starved while some going
    remarkably faster

[\[CHART\]]{.chart}

As an example, the above 2 charts give a rich view into addressing the
"fairness" question across all connections across 5 minutes (over
loopback). One could do similar analysis comparing connections across
different server addresses to look for issues with servers or groups of
servers (e.g. behind a bad routers for example).


### A detailed network behavior of the above example ###

For those inclined, the below explains in more detail the network
traffic generated with the above commands.

-   *The default options will result in 8 concurrent TCP connections
    established from the client to the server.*
    -   *The number of connections established can be optionally
        configured on the client via **-connections***
-   *The client will send 1GB of data (1073741824 bytes) across each
    connection using 64KB buffers. The data sent will come from a
    specifically formatted buffer that is used on both the client and
    server. This allows for accurate data validation.*
    -   *The number of bytes transferred over each connection can be
        optionally configured, [which must be equivalently set on both
        the client and server]{.underline}, via **-transfer***
-   *After sending data the client will wait for a confirmation response
    from the server that all data was received and verified.*
-   *After verifying success or failure of the connection, the
    connection is closed, and a new connection is established to the
    server (working to keep 8 concurrent connections alive).*
-   *The server accepts all connections from the client. ctsTraffic
    listening as a server accepts any number of connections from any
    number of clients.*
-   *Once accepted, the server will expect data to be sent from the
    client and start receiving.*
-   *The server, upon every receive completion, will verify the precise
    bit pattern in the bytes received. Any bytes received that do not
    match the expected pattern results in immediate failure and the
    server terminates the connection (immediately calls closesocket).*

# Scaling #

ctsTraffic was deliberation designed to scale: scaling down (it's been
used with very small IoT parts to view what good put looks like on a
device with very few resources), scaling up (it's been used to large
servers with 50 Gbps configurations to look at expected good put), and
scaling out (it's been tested with deployments of 10s of thousands of
concurrent connections; tested up to 500,000 connections).

We generally recommend scaling to match both the expected nominal
deployments and workloads as well as the 90% extreme deployments and
workloads. Using the options above to increase the numbers of concurrent
connections, ctsTraffic will naturally scale to the resources and
network pipes available.

_It's useful to note that this scaling comes with the same coding
models -- the same code runs which measures small IoT devices without
overloading their CPUs as severs with hundreds of cores that run 50Gbps
pipes. This all comes with our recommendation: using overlapped I/O with
the NT thread-pool and handling inline completions. It's a great
demonstration how the Windows OS will scale naturally._


# Testing for reliability #

We have added features over time which we found greatly helps in
measuring the reliability of a networking deployment. Below are examples
of combinations of options we have found to be particularly useful in
discovering issues in networking components and devices.


# Looking for data corruption #

While thankfully rare, we have found one method has been particularly
successful in discovering data corruption issues in hardware and
software stacks. This has found data corruption issues across a variety
of vendors and deployments. Interestingly in most cases it was only
ctsTraffic and only when ctsTraffic was run in this way was the data
corruption observed.

| **Server**                    | **Client**                            |
| ----------------------------- | ------------------------------------- |
| `ctsTraffic.exe `             | `ctsTraffic.exe `                     |
| `-listen:* `                  | `-target:<server> `                   |
| `-consoleverbosity:1 `        | `-consoleverbosity:1 `                |
| `-pattern:duplex `            | `-pattern:duplex `                    |

 
The unique bit here was running the "full duplex" pattern. This data
pattern will send and receive at line rate concurrently: sends posting
as quickly as they can post, receives posting as quickly as they can
post, all in parallel. This often results in making software work the
"hardest" as it must be tracking each TCP stream of data going in both
directions, at line rate. "At line rate" was also generally required.
With some 40 Gbps network devices we would only discover corruptions
when running the duplex pattern at full 40 Gbps bidirectional line rate.

Note that scaling the number of connections and transfer size of each
connection as one goes above 1Gbps does also help as it allows more time
for each connection. If a network deployment continues to have trouble
scaling to line rate, specifying the buffer sizes to 1MB can help
(**-buffer:1048576**).

## Randomizing buffer sizes ##

If one wants to work even harder to find data corruption bugs, one can
instruct ctsTraffic to randomize the buffer sizes used for each send and
receive request. This will often change the buffering patterns across a
networking stack, as TCP segments get created of different sizes which
can influence many other TCP factors, such as packet sizes and window
sizes.

The default value is 64k for all IO requests on all connections.
Randomizing buffer sizes can be done by specifying a range with square
brackets. The below is an example where each TCP connection would be
randomly choosing a buffer size to use for that connection between 1KB
and 1MB.

| **Server**                      | **Client**                            |
| ------------------------------- | ------------------------------------- |
| `ctsTraffic.exe `               | `ctsTraffic.exe `                     |
| `-listen:* `                    | `-target:<server> `                   |
| `-consoleverbosity:1 `          | `-consoleverbosity:1 `                |
| `-pattern:duplex `              | `-pattern:duplex `                    |
| `-buffer:[1024,1048576] `       | `-buffer:[1024,1048576] `             |

As noted previously, adjusting numbers of connections and the total
transfer size can be useful especially when working on deployments
beyond 1Gbps.

# Looking for connection establishment issues #

If one wants to work even harder to find issue in connection
establishment, there are options which can be used to force many more
connections to happen over time. The key to doing so is giving a much
larger value for the number of connections: -connection [as well
as]{.underline} giving a much smaller transfer size: -transfer:. The
combination tells ctsTraffic a) maintain a lot of concurrent
connections, and b) each connection should be very short-lived.

The result will cycle through a lot of connections very quickly.

| **Server**                    | **Client**                          |
| ----------------------------- | ----------------------------------- |
| `ctsTraffic.exe `             | `ctsTraffic.exe `                   |
| `-listen:* `                  | `-target:localhost `                |
| `-consoleverbosity:1 `        | `-consoleverbosity:1 `              |
| `-transfer:64 `               | `-transfer:64 `                     |
|                               | `-connections:100 `                 |


These commands when run as a quick test created the below output:

| TimeSlice | SendBps | RecvBps | In-Flight | Completed | NetError | DataError |
| --------: | ------: | ------: | --------: | --------: | -------: | --------: |
| 5.000 | 0 | 0 | 45 | 1555 | 0 | 0 |
| 10.000 | 19737 | 24055 | 0 | 3100 | 0 | 0 |
| 15.000 | 19200 | 23400 | 0 | 4600 | 0 | 0 |
| 20.000 | 19200 | 23400 | 0 | 6100 | 0 | 0 |
| 25.000 | 19200 | 23400 | 0 | 7600 | 0 | 0 |
| 30.001 | 19196 | 23395 | 0 | 9100 | 0 | 0 |
| 35.000 | 19203 | 23404 | 0 | 10600 | 0 | 0 |
| 40.000 | 19200 | 23400 | 0 | 12100 | 0 | 0 |
| 45.001 | 19196 | 23395 | 0 | 13600 | 0 | 0 |
| 50.000 | 19203 | 23404 | 0 | 15100 | 0 | 0 |
| 55.001 | 16623 | 20259 | 44 | 16372 | 154 | 0 |
| 60.001 | 9100 | 11092 | 33 | 17110 | 897 | 0 |
| 65.000 | 10037 | 12224 | 49 | 17868 | 1663 | 0 |


In the output from the run we can see in the Completed column that we
were quickly iterating through many thousands of successful connections.

One should also note that at around the 55 second mark we started seeing
errors. This is because of a TCP behavior called TIME-WAIT. Because the
default behavior for ctsTraffic is for the clients to issue a graceful
shutdown at the end of a connection, we create a 4-way FIN to gracefully
tear down that TCP connection. While this is a typical way clients and
servers terminate connections this can result in the **client's** tuple
(its IP and port) to be temporarily held in a "time-wait" state per RFC.
While in these states that port cannot be reused.

This can result in exhausting available ephemeral ports that the client
can choose from (even with some recent Windows TCP/IP stack fixes to
work harder to potentially reuse some of these ports).

We have options in ctsTraffic which can help to work around this issue:
one can tell ctsTraffic how to terminate each successful connection. To
avoid entering time-wait, we can tell ctsTraffic to force a RST to
shutdown the connection. An RST is a rude/abrupt way to end a connection
but is perfectly valid. The command line with this combination would
like this:

| **Server**                    | **Client**                           |
| ----------------------------- | ------------------------------------ |
| `ctsTraffic.exe `             | `ctsTraffic.exe `                    |
| `-listen:* `                  | `-target:localhost `                 |
| `-consoleverbosity:1 `        | `-consoleverbosity:1 `               |
| `-transfer:64 `               | `-transfer:64 `                      |
|                               | `-connections:100 `                  |
|                               | `-shutdown:rude `                    |


The **-shutdown** option (either 'graceful' or 'rude') will instruct the
client in how to end their connection (the server will always wait for
the client to initiate a closure and therefore never enter time-wait --
something we highly recommend to those building server software). As you
see in our simple example instead of seeing failures after about 16,000
connections, we were still creating successful connections after 20,000
connections.

| TimeSlice | SendBps | RecvBps | In-Flight | Completed | NetError | DataError |
| --------: | ------: | ------: | --------: | --------: | -------: | --------: |
| 0.001 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5.000 | 20087 | 24480 | 34 | 1566 | 0 | 0 |
| 10.001 | 19592 | 23879 | 0 | 3100 | 0 | 0 |
| 15.000 | 19203 | 23404 | 0 | 4600 | 0 | 0 |
| 20.000 | 19200 | 23400 | 0 | 6100 | 0 | 0 |
| 25.000 | 19200 | 23400 | 0 | 7600 | 0 | 0 |
| 30.001 | 19196 | 23395 | 0 | 9100 | 0 | 0 |
| 35.000 | 19203 | 23404 | 0 | 10600 | 0 | 0 |
| 40.000 | 19200 | 23400 | 0 | 12100 | 0 | 0 |
| 45.000 | 19200 | 23400 | 0 | 13600 | 0 | 0 |
| 50.000 | 19200 | 23400 | 0 | 15100 | 0 | 0 |
| 55.000 | 19200 | 23400 | 0 | 16600 | 0 | 0 |
| 60.000 | 19200 | 23400 | 0 | 18100 | 0 | 0 |
| 65.000 | 19200 | 23400 | 0 | 19600 | 0 | 0 |
| 70.000 | 19200 | 23400 | 0 | 21100 | 0 | 0 |


# UDP stream reliability #

ctsTraffic measures UDP flows through media streaming semantics -- how
most apps (especially client facing apps) use UDP datagrams. In our UDP
stream implementation, every datagram is tagged by number and by time.
Thus, the client receiving the stream of datagrams from the server can
accurately identify every dropped datagram as well as validating the
data integrity of each received datagram (the same bit-pattern analysis
occurs with UDP as with TCP to check for data corruption).

## A suggested starting point: measuring a common UDP stream ##

It's recommended to start with current stream behaviors -- to replicate
and measure those streams over time. To express this in scenario terms,
Netflix of often streaming much of its 2160p (4K) content at 15.26 Mbps,
though they recommend 25 Mbps availability.

We can accurately measure a deployment's ability to stream a 4K movie at
these rates. We will accurately send the specified stream and upon
receiving verify the data integrity of all datagrams, track all lost
frames (which would translate to lost packets), and track all repeated
frames (which can happen with various network topologies).

| **Server**                    | **Client**                                |
| ----------------------------- | ----------------------------------------- |
| `ctsTraffic.exe `             | `ctsTraffic.exe `                         |
| `-listen:* `                  | `-target:localhost `                      |
| `-protocol:udp `              | `-protocol:udp `                          |
| `-bitspersecond:25000000 `    | `-bitspersecond:25000000 `                |
| `-framerate:60 `              | `framerate:60 `                           |
| `-bufferdepth:1 `             | `-bufferdepth:1 `                         |
| `-streamlength:60 `           | `-streamlength:60 `                       |
| `-consoleverbosity:1 `        | `-consoleverbosity:1 `                    |
|                               | `-connections:1 `                         |
|                               | `-iterations:1 `                          |
|                               | `-statusfilename:udpclient.csv `          |
|                               | `-connectionfilename:udpconnection.csv `  |
|                               | `-jitterfilename:jitter.csv `             |

These options specify for the client to send a datagram to the server to
initiate a "connection" -- where the server will be sending 25Mbps of
data across 60 "frames" (datagrams) per second. Buffer depth is how much
of a time allowance the client will allow for variance in receiving
datagrams. 1 second is generally fine for most simulations.

The result of this test produces 3 log files, 2 similar to the TCP logs
and one which tracks jitter by comparing time stamps within the received
datagrams.

### Explaining the console output ###

As a sample run, the below is output from a quick test ran over loopback
(client and server were both run on my same machine). Note that the
-consoleverbosity: flag controls the type and detail of what it output
to the console (like with TCP, setting 0 turns off all output).

`C:\\Users\\kehor\\Desktop\\2.0.1.7\> **ctsTraffic.exe -target:localhost
 -protocol:udp -bitspersecond:25000000 -framerate:60 -bufferdepth:1
 -streamlength:60 -connections:1 -iterations:1 -consoleverbosity:1
 -statusfilename:udpclient.csv -connectionfilename:udpconnection.csv
 -jitterfilename:jitter.csv**`

`Configured Settings`

        Protocol: UDP
        Options: InlineIOCP SO\_RCVBUF(1048576)
        IO function: MediaStream Client
        IoPattern: MediaStream \<UDP controlled stream from server to client\>
        PrePostRecvs: 2
        PrePostSends: 1
        Level of verification: Connections & Data>
        Port: 4444
        Buffer used for each IO request: 52083 \[0xcb73\] bytes
        Total transfer per connection: 187498800 bytes
        UDP Stream BitsPerSecond: 25000000 bits per second
        UDP Stream FrameRate: 60 frames per second
        UDP Stream BufferDepth: 1 seconds
        UDP Stream StreamLength: 60 seconds (3600 frames)
        UDP Stream FrameSize: 52083 bytes
        Connecting out to addresses:
               [::1]:4444
               127.0.0.1:4444
        Binding to local addresses for outgoing connections:
               0.0.0.0
               ::
        Connection limit (maximum established connections): 1 \[0x1\]
        Connection throttling rate (maximum pended connection attempts): 1000 [0x3e8]
        Total outgoing connections before exit (iterations \* concurrent connections) : 1 [0x1]

` Legend: `

    * TimeSlice - (seconds) cumulative runtime
    * Streams - count of current number of UDP streams
    * Bits/Sec - bits streamed within the TimeSlice period
    * Completed Frames - count of frames successfully processed within the TimeSlice
    * Dropped Frames - count of frames that were never seen within the TimeSlice
    * Repeated Frames - count of frames received multiple times within the TimeSlice
    * Stream Errors - count of invalid frames or buffers within the TimeSlice

| TimeSlice | Bits/Sec | Streams | Completed | Dropped | Repeated | Errors |
| --------: | -------: | ------: | --------: | ------: | -------: | -----: |
| 5.000 | 290 | 1 | 240 | 0 | 0 | 0 |
| 10.000 | 24999840 | 1 | 300 | 0 | 0 | 0 |
| 15.000 | 24999840 | 1 | 300 | 0 | 0 | 0 |
| 20.000 | 24999840 | 1 | 300 | 0 | 0 | 0 |
| 25.000 | 25004840 | 1 | 300 | 0 | 0 | 0 |
| 30.000 | 24999840 | 1 | 300 | 0 | 0 | 0 |
| 35.000 | 24999840 | 1 | 300 | 0 | 0 | 0 |
| 40.000 | 24999840 | 1 | 300 | 0 | 0 | 0 |
| 45.001 | 24999840 | 1 | 300 | 0 | 0 | 0 |
| 50.000 | 25004840 | 1 | 300 | 0 | 0 | 0 |
| 55.000 | 24999840 | 1 | 300 | 0 | 0 | 0 |
| 60.000 | 25004840 | 1 | 300 | 0 | 0 | 0 |
| 61.273 | 327566 | 0 | 60 | 0 | 0 | 0 |

`Historic Connection Statistics (all connections over the complete lifetime)`

`SuccessfulConnections [1] NetworkErrors [0] ProtocolErrors [0]`

` Total Bytes Recv : 187498800`

` Total Successful Frames : 3600`

` Total Dropped Frames : 0`

` Total Duplicate Frames : 0`

` Total Error Frames : 0`

` Total Time : 61273 ms.`


The banner under **Configured Settings** shows default settings with how
the streaming parameters were turned into datagram rates.

-   Default is to establish UDP session using OVERLAPPED I/O with IO
    completion ports managed by the NT threadpool on the client
    receiving datagrams (and just blocking sends with an NT threadpool
    timer on the server), by default handling inline successful
    completions.
-   By default, ctsTraffic will configure the client to set the socket
    option SO\_RCVBUF to 1 MB. This pre-allocates 1MB of buffer in
    Winsock (afd.sys), which is reasonable and suggested for client apps
    receiving a stream.
    -   *This buffer allows for timing variance with a general purpose
        OS for IO completions*
    -   *This is configurable should one want to emulate other types of
        apps: -RecvBufValue*
-   Default is to pre-post 2 pended receives (keeping 2 OVERLAPPED
    receives pended waiting for data). As datagrams do not have in-order
    guarantees, ctsTraffic accounts for tracking the order of received
    datagrams (as any app receiving UDP datagrams should).
    -   *This is configurable should one want to push a much greater
        throughput over a single UDP connection: -PrePostRecvs*
-   Shows the details of how the stream translates into individual
    datagrams being sent and received.
    -   Buffer is the buffer size used with each receive request. It
        generally equates to the calculated FrameSize
    -   Total transfer is the calculated number of bytes when
        multiplying the bits/second by the total amount of time the
        stream will run (and then \* 8 to convert to bytes)
    -   Shows the input BitsPerSecond, FrameRate, BufferDepth, and
        StreamLength
    -   Shows the calculated FrameSize -- the number of bytes that will
        be sent (FrameRate) number of times per second
        -   For example, in the above session where we're streaming
            25Mbps at 60 frames per second, the server will be sending
            52,083 bytes split evenly across 60 times per second.
-   Shows all resolved addresses which will be used in a round-robin
    fashion as connections are made.
    -   *Target IP addresses is configurable using one or more -Target
        options, specifying a name or IP address servers which to
        connect.*
-   Shows that will use ephemeral binding (binding to 'any' address of
    all zeros lets the TCP/IP stack choose the best route to each target
    address).
    -   *The local addresses to use for outbound connections is
        configurable: -bind.*
-   Default is to keep 1 UDP session ("connection") established and
    streaming data.
    -   *The number of connections to establish is configurable:
        -connections.*
-   Default is to indefinitely continue making connections as individual
    connections complete -- maintaining 8 connections at all time.
    -   *The total number of connections is configurable: -connections:
        and -iterations:. The total connections is the product of
        connections \* iterations.*
    -   *e.g. -connections:100 -iterations:10 will iterate 10 times over
        100 connections for a total of 1000 connections, at which point
        ctsTraffic will gracefully exit.*

**-consoleverbosity:1** will output an aggregate status at each time
slice. The default time slice is every 5 seconds; the time slice is
configurable: -statusUpdate. At every 5 seconds, a line will be output
communicating the following aggregate information:

-   *TimeSlice*: the time window in seconds with millisecond precision,
    starting from when ctsTraffic was launched (since -statusUpdate can
    set update frequency in milliseconds)
-   *Bits/Sec*: the calculated received bits/sec [within that specific
    time slice]{.underline}
-   *Streams*: the number of concurrent streams running at the time the
    slice was recorded
-   *Completed*: the number of successfully verified **frames** received
    [within that specific time slice]{.underline}
-   *Dropped*: the number of verified **dropped frames** [within that
    specific time slice]{.underline}
-   *Repeated*: the number of verified **repeated frames** [within that
    specific time slice]{.underline}
-   *Errors*: the number of **frames** with data errors found [within that
    specific time slice]{.underline}


### Explaining the generated log files ###

In the same sample as above, three log files were created due to the
following command line options: "**-statusfilename:udpclient.csv
-connectionfilename:udpconnection.csv -jitterfilename:jitter.csv**". The
csv extension informed ctsTraffic output the files in a comma-separated
values format (any other extension would be written as a line of text).

#### StatusFilename ###

The status file writes out the same information to a csv as is written
to console with the above "**-consoleverbosity:1**" option set. This is
useful for later analysis, notably in an application like Excel.
Imported into Excel, 25 seconds worth of data would look like this:

| TimeSlice | Bits/Sec | Streams | Completed | Dropped | Repeated | Errors |
| --------: | -------: | ------: | --------: | ------: | -------: | -----: |
|  5        |      290        |   1     |       240     |       0      |      0       |      0 |
|  10       |      24999840   |   1     |       300     |       0      |      0       |      0 |
|  15       |      24999840   |   1     |       300     |       0      |      0       |      0 |
|  20       |      24994841   |   1     |       300     |       0      |      0       |      0 |
|  25       |      25004840   |   1     |       300     |       0      |      0       |      0 |
|  30       |      24999840   |   1     |       300     |       0      |      0       |      0 |
|  35       |      24999840   |   1     |       300     |       0      |      0       |      0 |
|  40       |      24994841   |   1     |       300     |       0      |      0       |      0 |
|  45.001   |      24999840   |   1     |       300     |       0      |      0       |      0 |
|  50       |      25004840   |   1     |       300     |       0      |      0       |      0 |
|  55       |      24994841   |   1     |       300     |       0      |      0       |      0 |
|  60       |      24999840   |   1     |       300     |       0      |      0       |      0 |
|  61.273   |      327566     |   0     |       60      |       0      |      0       |      0 |

- This becomes useful as Excel can quickly give richer views into our
data.
- With this view when looking at a long stream we can look at patterns of
data loss (Dropped \> 0), router issues (Repeated \> 0), and data
integrity issues (Errors \> 0).
- Additionally, Excel does quick graphing, which can be some of the most
powerful ways to view data.
- Over loopback the client received a very steady 25 Mbps. As one scales
this can certainly change if there are issues with the network supplying
a consistent stream of data.


[\[CHART\]]{.chart}

[\[CHART\]]{.chart}


#### ConnectionFilename ###

The status file writes out per connection information to a csv (this
would be the same as what is written to console with
"**-consoleverbosity:3**" option set). This is useful for later analysis
when wanting to look at patterns across a long test run.

This is similar to the TCP connection view, this time with aggregate
data points for each connection.

For a sample, I ran the above 25Mbps run with 10 concurrent UDP streams
(-connections:10). Below is the connection output:

![](media/image3.emf){width="6.5in" height="1.3923611111111112in"}

In this log file we can see individual UDP sessions ("connections")
recorded.

-   The UDP tuple information (the local address and port, the remote
    address and port)
-   Received bits/second aggregate across the stream
-   Successfully Completed (received and validated) frames (datagrams)
-   All verified Dropped datagrams for that stream
-   All verified Repeated datagrams for that stream
-   All verified data Errors in received datagrams for that stream
-   The end Result of that stream
    -   e.g. were there were Winsock errors which prevented that session
        from continuing to try to receive datagrams
-   The "ConnectionId" -- a shared GUID from the server and client --
    useful when scenarios need to correlate client logs to server logs
    (even more useful when going through NATs and load balancers where
    addresses don't always provide unique reference points)

As with the Status file analysis, Excel can give deeper insight into the
test run. For example:

-   One could look at failure patterns: did they occur randomly or in
    specific groups of time
-   One could look at which remote addresses had more failures
-   One could look at patterns in Bits/Sec, correlating with remote
    servers to see which servers were more performant


#### JitterFilename ###

When making a test run with just a **single UDP connection**, the client
can also track jitter information. This is collected by tracking every
individual datagram received and looking at the times stamped on it by
the server. Even though the client and server are not time synchronized,
the client can still analyze the latency deltas by using the first
datagram received as its baseline and calculating gaps. Because the
client and server had the same parameters specified, the client know the
number of milliseconds that the server would have been waiting between
calls to send(). By subtracting the known timer value when the server
was waiting between sends it can calculate the time between the actual
send and the resulting receive.

As a trivial example, here is jitter being tracked over loopback for the
first 30 datagrams received:

![](media/image4.emf){width="6.5in" height="4.928472222222222in"}

The chart shows the sender and receiver QueryPerformanceCounter and
QueryPerformanceFrequency values which ctsTraffic stamped in the
datagram payload. This leads to being able to calculate "Estimated
Received Datagram Time In Flight". You'll note that being over loopback
and given optimizations the math resulted in the time in flight being
negative .


# Streaming over Wi-Fi #

As a more interesting example, running the above 25Mbps stream from a
small Surface laptop over Wi-Fi to another machine also connected over
Wi-Fi shows more diverse data.

The status output now shows more variance in throughput as well as
infrequent packet drops. Because this was a shorter run (only 60
seconds) and I wanted to look into greater detail, I set
-statusUpdate:500 so I had a twice/second updated view of throughput and
packet drops.

Here's a sample of the first 10 seconds:

![](media/image5.emf){width="5.638194444444444in"
height="4.595138888888889in"}

You'll notice now that we have data every ½ second (500 ms). We can see
we expected to receive 30 individual frames within each ½ second and
there were bursts when datagrams were dropped.

Graphing always helps .

[\[CHART\]]{.chart}

This is showing bits/second across the entire 60 seconds of the stream.
Because this is Excel I could also quickly do =AVERAGE(), =MIN(), and
=MAX() to get a slightly better view into the data:

|  AVERAGE  | 24091960   |
| --------- | ---------- |
|  MIN      | 23333184   |
|  MAX      | 24850735   |

Just as useful we can look at Jitter data in a more relevant scenario --
here are the first 20 frames:

![](media/image6.emf){width="6.5in" height="3.238888888888889in"}

We can see that variance drifted quite a bit, with larger gaps with a
few negative gaps as datagrams arrived in bursts. Graphing this
information gives us insightful views into the variance between
datagrams received:

[\[CHART\]]{.chart}

With these views we can now see the variance distribution over this 60
second 25 Mbps stream of datagrams.
