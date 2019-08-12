# ElasticLog

## abstract 
Elastic Log is a asynchronization log written by C++. It is an efficient (106 w/s entries per second), scalable log service in Linux. In short, there are two features supported by my ElasticLog:  
+ **Optimize the UTC generator**  
ElasticLog supports UTC time generator to invoke `localtime` every minute. it's much more efficient for users compared with invoking `local_time` every time we generate a log entry.
+ **Scalable**  
ElasticLog is based on double linked list. Each link list node is a cell buffer. The default size of the double linked list is 3. It supports more cell buffers at runtime only if the whole linked list size is under memory limit user set. so, when the producers are producing fast and consumer is comsuming slowly, ElasticLog will generator new cell buffer to avoid producer block.
