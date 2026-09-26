use servercore::{
    block_on,
    web::{Body, BodyOptions, Event, EventKind, HttpServer, Options, ResponseHead},
    web_data::{Multipart, MultipartOptions, PartKind},
    Error, Result,
};

// Process each event immediately: no vector of file chunks is accumulated.
// A real sink can write to its own bounded queue; never use filename as a path.
fn drain(parser: &mut Multipart, total: &mut u64) -> Result<bool> {
    loop {
        match parser.read() {
            Ok(Some(event)) => {
                if event.kind() == PartKind::Data {
                    *total += event.data().len() as u64;
                }
                // Dropping this event returns its parser byte charge.
            }
            Ok(None) => return Ok(true),
            Err(Error::WOULD_BLOCK) => return Ok(false),
            Err(error) => return Err(error),
        }
    }
}

async fn consume(body: &mut Body, content_type: &str) -> Result<u64> {
    let mut parser = Multipart::new(content_type, MultipartOptions::default())?;
    let mut total = 0;
    while let Some(chunk) = body.next().await? {
        let mut remaining = chunk.bytes();
        while !remaining.is_empty() {
            drain(&mut parser, &mut total)?;
            // A single body chunk can exceed parser capacity: retry its suffix.
            let consumed = parser.feed(remaining)?;
            remaining = &remaining[consumed..];
        }
        drain(&mut parser, &mut total)?;
        // Drop chunk before awaiting more body input, returning upload capacity.
    }
    parser.finish()?;
    if !drain(&mut parser, &mut total)? {
        return Err(Error::INVALID_FORMAT);
    }
    Ok(total)
}

fn main() -> Result<()> {
    let port = std::env::args().nth(1).and_then(|value| value.parse().ok()).unwrap_or(8080);
    let mut server = HttpServer::new(&Options { port, ..Default::default() })?;
    server.set_body_limits(&BodyOptions { max_buffered_bytes: 64 * 1024,
        max_body_bytes: 128 * 1024 * 1024 })?;
    server.stream_route("POST", "/upload")?;
    server.start()?;
    println!("Listening on 127.0.0.1:{}; POST multipart/form-data to /upload", server.port());
    block_on(async {
        loop {
            let event = server.next().await?;
            // A client that fails mid-response ends only its own request.
            if let Err(error) = handle(event).await { eprintln!("request failed: {error}"); }
        }
    })
}

async fn handle(mut event: Event) -> Result<()> {
    if event.kind()? != EventKind::Request { return Ok(()); }
    let content_type = event.request()?.headers.get("content-type")
        .and_then(|value| std::str::from_utf8(value).ok()).unwrap_or("").to_owned();
    let mut body = event.body()?;
    let response = event.response()?;
    drop(event);
    match consume(&mut body, &content_type).await {
        Ok(total) => {
            let reply = format!("validated {total} payload bytes\n");
            response.start_async(&ResponseHead { content_length: Some(reply.len() as u64),
                ..Default::default() }).await?;
            response.write_all(reply.as_bytes()).await?;
            response.finish_async().await
        }
        Err(error) => {
            // Cancel stops the underlying upload and aborts its response.
            // A production application can choose a bounded error-response
            // policy instead; no partially parsed upload is committed here.
            body.cancel();
            eprintln!("upload rejected: {error}");
            Ok(())
        }
    }
}
