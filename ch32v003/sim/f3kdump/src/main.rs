// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// Every packet of a freedj-3000 capture log through freedj-3000's own parser, one canonical line each, for
// diffing against freedj-nano's sim/prolink_replay (fw/prolink.c).
use opendeck_link::prodj::*;
fn parse_line(line: &str) -> Option<(u16, Vec<u8>)> {
    let i = line.find("ProDJ rx :")?;
    let rest = &line[i + 10..];
    let port: u16 = rest.split(' ').next()?.parse().ok()?;
    let open = rest.find('[')?; let close = rest.rfind(']')?;
    let bytes = rest[open + 1..close].split(',').map(|h| u8::from_str_radix(h.trim(), 16)).collect::<Result<Vec<_>, _>>().ok()?;
    Some((port, bytes))
}
fn main() {
    let path = std::env::args().nth(1).expect("log");
    for line in std::fs::read_to_string(path).unwrap().lines() {
        let Some((port, d)) = parse_line(line) else { continue };
        match (port, ProDjLink::packet_type(&d)) {
            (50001, Some(PKT_BEAT)) => match ProDjLink::parse_beat(&d) {
                Some(b) => println!("beat player {} bpm {:.2} bib {}", b.player, b.bpm * b.pitch, b.beat_in_bar),
                None => println!("beat rejected {}", d.len()),
            },
            (50002, Some(PKT_STATUS)) => match ProDjLink::parse_status(&d) {
                Some(s) => {
                    let p = if (0.25..=4.0).contains(&s.pitch) { s.pitch } else { 1.0 };
                    println!("status player {} playing {} master {} bpm {:.2} pitch {:.4} handoff {} syncN {}", s.player,
                             s.playing as u8, s.master as u8, s.bpm.unwrap_or(0.0), p, s.handoff_to.unwrap_or(0xff), s.sync_counter)
                }
                None => println!("status rejected {}", d.len()),
            },
            (50000, Some(PKT_ANNOUNCE)) => match ProDjLink::parse_announce(&d) {
                Some((p, ip)) => println!("announce player {} ip {}", p, ip),
                None => println!("announce rejected {}", d.len()),
            },
            (port, t) => println!("other port {} type {:?} len {}", port, t, d.len()),
        }
    }
}
