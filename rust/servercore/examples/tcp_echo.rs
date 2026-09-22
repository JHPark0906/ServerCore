use servercore::{
    block_on,
    net::{EventKind, Options, TcpServer},
};

fn main() -> servercore::Result<()> {
    let port = std::env::args()
        .nth(1)
        .and_then(|value| value.parse().ok())
        .unwrap_or(7777);
    let mut server = TcpServer::new(&Options {
        port,
        ..Default::default()
    })?;
    server.start()?;
    println!("Binary TCP echo on 127.0.0.1:{}", server.port());
    // This small example handles one connection at a time. A production game
    // server can spawn bounded tasks on its chosen Rust executor instead.
    block_on(async {
        loop {
            let mut connection = server.accept().await?;
            loop {
                let event = connection.next().await?;
                if event.kind()? == EventKind::Closed {
                    break;
                }
                connection.send_async(event.bytes()).await?;
            }
        }
    })
}
