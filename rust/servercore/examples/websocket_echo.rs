use servercore::{
    block_on,
    web::{HttpServer, Options, WebSocketKind},
};

fn main() -> servercore::Result<()> {
    let mut server = HttpServer::new(&Options {
        port: 8080,
        ..Default::default()
    })?;
    server.websocket_pattern("/rooms/{id}")?;
    server.start()?;
    println!("WebSocket echo at ws://127.0.0.1:8080/rooms/lobby");
    block_on(async {
        loop {
            let mut upgrade = server.next().await?;
            let mut socket = upgrade.socket()?;
            println!(
                "joined room {}",
                upgrade.request()?.parameter("id").unwrap_or("")
            );
            drop(upgrade);
            loop {
                let event = socket.next().await?;
                match event.kind()? {
                    WebSocketKind::Text => socket.send_text_async(event.text()?).await?,
                    WebSocketKind::Binary => socket.send_binary_async(event.bytes()).await?,
                    WebSocketKind::Closed => break,
                }
            }
        }
    })
}
