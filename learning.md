#### Asio的端点

之前我一直是有误解的，认为`ip::tcp::endpoint`中的参数可以随便填充，不过我当时的立即是有问题的

> 有三种方式来让你建立一个端点：
>
> - *endpoint()*：这是默认构造函数，某些时候可以用来创建UDP/ICMP socket。
> - *endpoint(protocol, port)*：这个方法通常用来创建可以接受新连接的服务器端socket。
> - *endpoint(addr, port)*:这个方法创建了一个连接到某个地址和端口的端点。