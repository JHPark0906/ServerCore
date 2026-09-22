use servercore::{
    block_on,
    web::{EventKind, HttpServer, Options, ResponseHead},
};

fn main() -> servercore::Result<()> {
    let port = std::env::args()
        .nth(1)
        .and_then(|value| value.parse().ok())
        .unwrap_or(8080);
    let mut server = HttpServer::new(&Options {
        port,
        ..Default::default()
    })?;
    server.route_pattern("POST", "/echo/{id}")?;
    server.route("GET", "/health")?;
    server.start()?;
    println!(
        "Listening on 127.0.0.1:{}; POST /echo/example",
        server.port()
    );
    block_on(async {
        loop {
            let mut event = server.next().await?;
            if event.kind()? != EventKind::Request {
                continue;
            }
            let response = event.response()?;
            let request = event.request()?;
            let body = if request.target == "/health" {
                b"ready".as_slice()
            } else {
                request.body
            };
            response
                .start_async(&ResponseHead {
                    content_length: Some(body.len() as u64),
                    ..Default::default()
                })
                .await?;
            response.write_all(body).await?;
            response.finish_async().await?;
        }
    })
}
