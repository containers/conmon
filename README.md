# conmon

An OCI container runtime monitor.

Conmon is a monitoring program and communication tool between a container manager (like [podman](https://podman.io/) or [CRI-O](https://cri-o.io/)) and an OCI runtime (like [runc](https://github.com/opencontainers/runc) or [crun](https://github.com/giuseppe/crun)) for a single container.

Upon being launched, it double-forks to daemonize and detach from the parent that launched it. It then launches the runtime as its child.  This allows managing processes to die in the foreground, but still be able to watch over and connect to the child process (the container).

While the container runs, conmon does two things:
* Provides a socket for attaching to the container, holding open the container's standard streams and forwarding them over the socket.
* Writes the contents of the container's streams to a log file (or to the systemd journal) so they can be read after the container's death.

Finally, upon the containers death, conmon will record its exit time and code to be read by the managing programs.

Written in C and designed to have a low memory footprint, conmon is intended to be run by a container managing library. Essentially, conmon is the smallest daemon a container can have.
