[![LinkedIn](https://img.shields.io/badge/LinkedIn-0A66C2?style=for-the-badge&logo=linkedin&logoColor=white)](https://www.linkedin.com/in/alexisdrobles/)
[![Email](https://img.shields.io/badge/Email-D14836?style=for-the-badge&logo=gmail&logoColor=white)](mailto:alexisrobles2025@u.northwestern.edu)
[![GitHub](https://img.shields.io/badge/GitHub-181717?style=for-the-badge&logo=github&logoColor=white)](https://github.com/al3xisrobles)
[![Personal Website](https://img.shields.io/badge/Portfolio-FF7139?style=for-the-badge&logo=googlechrome&logoColor=white)](https://alexis-robles.com)

# Fast User-Space Network Packet Filter for HFT

**How can modern OS and hardware features be used to build ultra-low-latency trading infrastructure?**

This project aims to build a high-performance, user-space network packet filter optimized for low-latency trading environments. Traditional network packet filtering solutions rely heavily on kernel-space processing, introducing unnecessary context switch overhead, system call latency, and buffer copying inefficiencies. By bypassing the kernel’s traditional networking stack and implementing user-space packet filtering, this project will explore the fundamental trade-offs in networking performance and HFT infrastructure optimizations.

Key features include
- Zero-copy packet capture using AF_PACKET, PF_RING, or a similar mechanism.
- Optimized filtering pipeline to discard unwanted packets with minimal latency.
- Batch processing and SIMD optimizations to enhance throughput.
- Kernel bypass techniques (io_uring, DPDK, or XDP) for further speed improvements.

---

## Introduction

In HFT, nanoseconds matter. Every microsecond of delay between receiving market data and placing an order can determine profitability. As a result, packet filtering efficiency is critical—traders must process high-velocity financial data feeds in real-time, without waiting for the operating system’s traditional networking stack to handle incoming packets.

This project implements a high-performance user-space packet filter, which skips the overhead of system calls, kernel context switches, and buffer copies. The standard Berkeley Packet Filter (BPF) and its successor, eBPF/XDP, provide efficient in-kernel packet filtering, but even these approaches suffer from context switch penalties. Instead, user-space solutions such as AF_PACKET (mmap-based packet capture), PF_RING, io_uring, and DPDK provide direct access to network interfaces, enabling microsecond-scale packet processing.

The core idea behind this implementation is to:
1. Capture packets directly in user space using a kernel-bypass approach.
2. Filter packets efficiently using a minimal processing pipeline, potentially leveraging SIMD (AVX, SSE) to maximize CPU parallelism.
3. Batch process packets to avoid the syscall overhead of per-packet operations.

---

## Methods

TODO

---

## References

#### **Academic Research Papers**

- Rizzo, Luigi. *“netmap: A Novel Framework for Fast Packet I/O.”* *USENIX ATC*, 2012.
- Belay, Adam, et al. *“IX: A Protected Dataplane Operating System.”* *Proceedings of the 11th USENIX Symposium on Operating Systems Design and Implementation (OSDI)*, 2014, pp. 49–65.
- Peter, Simon et al. *“Arrakis: The Operating System is the Control Plane.”* *OSDI*, 2014.
- Emmerich, Paul, et al. *“User Space Network Drivers.”* *Proceedings of the 15th ACM/IEEE Symposium on Architectures for Networking and Communications Systems (ANCS)*, 2019, pp. 1–13.
- Høiland-Jørgensen, Toke, et al. *“The eXpress Data Path: Fast Programmable Packet Processing in the Operating System Kernel.”* *Proceedings of the 14th International Conference on Emerging Networking EXperiments and Technologies (CoNEXT)*, 2018, pp. 54–66.
- Sharaf, Mohamed, et al. *“Extended Berkeley Packet Filter (eBPF) – An Application Perspective.”* *IEEE Access*, vol. 10, 2022, pp. 23344–23357.
- Castillon du Perron, Quentin, et al. *“Understanding Delays in AF_XDP-based Applications.”* *arXiv preprint arXiv:2401.03456*, 2024.
- Bhattacherjee, Debopam, et al. *“A Bird’s Eye View of the World’s Fastest Networks.”* *Proceedings of the 20th ACM Internet Measurement Conference (IMC)*, 2020, pp. 130–144.
- Planeta, Dominik, et al. *“CoRD: Converged RDMA Dataplane.”* *arXiv preprint arXiv:2305.08976*, 2023.
- Kogias, Eleftherios, et al. *“MachNet: Fast Userspace Networking for Cloud VMs.”* *arXiv preprint arXiv:2502.11322*, 2025.
- Farshin, Alireza, et al. *“PacketMill: Toward Per-Core 100-Gbps Networking.”* *Proceedings of the 26th International Conference on Architectural Support for Programming Languages and Operating Systems (ASPLOS)*, 2021, pp. 241–255.

#### **Technical Blog Posts & Documentation**
- ntop. *“PF_RING: A High-Speed Packet Capture Framework.”* ntop.org, 2023.
- Databento Engineering. *“Kernel Bypass for Trading: DPDK, io_uring, and Alternatives.”* 2023.
- Red Hat Developer. *“Why You Should Use io_uring for Network I/O.”* Red Hat, 2023.
- Igalia Blog. *“A Brief Introduction to XDP and eBPF.”* 2023.
- HackerNoon. *“The HFT Developer’s Guide: Six Key Components for Low Latency and Scalability.”* 2022.
- SiS Dev Blog. *“Best Practices on HFT Low-Latency Software.”* 2023.

---

## TODO
- Implement a **user-space packet capture framework**.
- Compare **AF_PACKET, PF_RING, io_uring, and DPDK** for kernel bypass.
- Optimize filtering using **SIMD (AVX, SSE) and batch processing**.
- Evaluate performance across **different network conditions and workloads**.

---

## License
This project is open-source and available under the MIT License.
