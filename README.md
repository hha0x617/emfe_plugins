# emfe_plugins

Guest-CPU plugins for the `emfe` emulator framework. Each subdirectory is a
self-contained plugin that exposes the `emfe` C ABI.

| Plugin | Target |
|--------|--------|
| `mc6809` | Motorola 6809 (wraps the `em6809` Rust crate) |
| `mc68030` | Motorola 68030 |
| `z8000` | Zilog Z8000 family (Z8001/Z8002/Z8003/Z8004) |
| `em8` | Small educational CPU |
| `rv32ima` | RISC-V RV32IMA |
| `api` | Shared C ABI headers |

Sample guest programs (e.g. Tiny Forth for MC6809) live under each plugin's
`examples/` directory.

## License

Licensed under either of

 * Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or
   <http://www.apache.org/licenses/LICENSE-2.0>)
 * MIT license ([LICENSE-MIT](LICENSE-MIT) or
   <http://opensource.org/licenses/MIT>)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally
submitted for inclusion in the work by you, as defined in the Apache-2.0
license, shall be dual licensed as above, without any additional terms or
conditions.
