/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! RTP video depacketization and per-frame reassembly for remote WebRTC tracks.

use std::collections::BTreeMap;

use webrtc::rtp::codecs::h264::H264Packet;
use webrtc::rtp::codecs::vp8::Vp8Packet;
use webrtc::rtp::codecs::vp9::Vp9Packet;
use webrtc::rtp::packet::Packet;
use webrtc::rtp::packetizer::Depacketizer;

const MAX_BUFFERED_PACKETS: usize = 128;
const MAX_WAIT_BEFORE_DROP: i64 = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VideoCodec {
    Vp8,
    Vp9,
    H264,
    Av1,
}

impl VideoCodec {
    #[must_use]
    pub fn from_mime_type(mime_type: &str) -> Option<Self> {
        let base = mime_type.split(';').next().unwrap_or(mime_type).trim();
        if base.eq_ignore_ascii_case("video/vp8") {
            Some(Self::Vp8)
        } else if base.eq_ignore_ascii_case("video/vp9") {
            Some(Self::Vp9)
        } else if base.eq_ignore_ascii_case("video/h264") {
            Some(Self::H264)
        } else if base.eq_ignore_ascii_case("video/av1") {
            Some(Self::Av1)
        } else {
            None
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct AssembledVideoFrame {
    pub data: Vec<u8>,
    pub rtp_timestamp: u32,
    pub sequence_number: u16,
    pub payload_type: u8,
    pub is_keyframe: bool,
}

#[derive(Debug, Default)]
pub struct PushOutcome {
    pub frames: Vec<AssembledVideoFrame>,
    pub request_pli: bool,
}

#[derive(Default)]
struct SeqUnwrapper {
    last_raw: Option<u16>,
    last_unwrapped: i64,
}

impl SeqUnwrapper {
    fn unwrap(&mut self, seq: u16) -> i64 {
        let Some(prev_raw) = self.last_raw else {
            self.last_raw = Some(seq);
            self.last_unwrapped = i64::from(seq);
            return self.last_unwrapped;
        };
        let delta = seq.wrapping_sub(prev_raw) as i16;
        let unwrapped = self.last_unwrapped + i64::from(delta);
        if unwrapped > self.last_unwrapped {
            self.last_raw = Some(seq);
            self.last_unwrapped = unwrapped;
        }
        unwrapped
    }
}

pub struct VideoFrameAssembler {
    codec: VideoCodec,
    unwrapper: SeqUnwrapper,
    buffer: BTreeMap<i64, Packet>,
    last_emitted_seq: Option<i64>,
    waiting_for_keyframe: bool,
    cached_h264_sps: Option<Vec<u8>>,
    cached_h264_pps: Option<Vec<u8>>,
}

impl VideoFrameAssembler {
    #[must_use]
    pub fn new(codec: VideoCodec) -> Self {
        Self {
            codec,
            unwrapper: SeqUnwrapper::default(),
            buffer: BTreeMap::new(),
            last_emitted_seq: None,
            waiting_for_keyframe: true,
            cached_h264_sps: None,
            cached_h264_pps: None,
        }
    }

    pub fn push_packet(&mut self, packet: Packet) -> PushOutcome {
        let mut outcome = PushOutcome::default();
        if packet.payload.is_empty() {
            return outcome;
        }

        let seq = self.unwrapper.unwrap(packet.header.sequence_number);
        if self.last_emitted_seq.is_some_and(|last| seq <= last) || self.buffer.contains_key(&seq) {
            return outcome;
        }
        self.buffer.insert(seq, packet);

        if self.buffer.len() > MAX_BUFFERED_PACKETS {
            self.drop_oldest_timestamp_group();
            self.waiting_for_keyframe = true;
            outcome.request_pli = true;
        }

        self.drain_ready_frames(&mut outcome);
        outcome
    }

    fn newest_seq(&self) -> Option<i64> {
        self.buffer.keys().next_back().copied()
    }

    fn drop_oldest_timestamp_group(&mut self) {
        let Some((&first_seq, first_pkt)) = self.buffer.iter().next() else {
            return;
        };
        let ts = first_pkt.header.timestamp;
        let mut drop_seqs = Vec::new();
        for (expected, (&seq, pkt)) in (first_seq..).zip(self.buffer.iter()) {
            if seq != expected || pkt.header.timestamp != ts {
                break;
            }
            drop_seqs.push(seq);
        }
        let Some(&last_dropped) = drop_seqs.last() else {
            return;
        };
        for seq in drop_seqs {
            self.buffer.remove(&seq);
        }
        self.last_emitted_seq = Some(self.last_emitted_seq.map_or(last_dropped, |prev| prev.max(last_dropped)));
    }

    fn drain_ready_frames(&mut self, outcome: &mut PushOutcome) {
        while let Some((&first_seq, first_pkt)) = self.buffer.iter().next() {
            let newest = self.newest_seq().unwrap_or(first_seq);

            // Wait briefly for a missing sequence number right before the head unless we have moved on.
            if let Some(last_emitted) = self.last_emitted_seq {
                if first_seq > last_emitted + 1 && newest - (last_emitted + 1) < MAX_WAIT_BEFORE_DROP {
                    break;
                }
                if first_seq > last_emitted + 1 {
                    self.waiting_for_keyframe = true;
                    outcome.request_pli = true;
                }
            }

            let target_ts = first_pkt.header.timestamp;
            let mut seqs = Vec::new();
            let mut expected = first_seq;
            let mut has_gap = false;
            let mut has_marker = false;
            let mut saw_later_timestamp = false;

            for (&seq, pkt) in &self.buffer {
                if seq != expected {
                    if pkt.header.timestamp == target_ts {
                        has_gap = true;
                    } else {
                        saw_later_timestamp = true;
                    }
                    break;
                }
                if pkt.header.timestamp != target_ts {
                    saw_later_timestamp = true;
                    break;
                }
                seqs.push(seq);
                expected += 1;
                if pkt.header.marker {
                    has_marker = true;
                    break;
                }
            }

            if has_gap {
                if newest - expected < MAX_WAIT_BEFORE_DROP {
                    break;
                }
                for seq in seqs {
                    self.buffer.remove(&seq);
                }
                self.last_emitted_seq = Some(expected - 1);
                self.waiting_for_keyframe = true;
                outcome.request_pli = true;
                continue;
            }

            if !has_marker {
                // If a packet for a newer frame arrived without a marker closing this timestamp, treat the old group
                // as truncated (or a marker-less tail only if the newer frame didn't skip seqs? In WebRTC RTP video,
                // every complete frame's final packet carries marker=true; lacking marker indicates loss).
                if saw_later_timestamp && newest - first_seq >= MAX_WAIT_BEFORE_DROP {
                    for seq in seqs {
                        self.buffer.remove(&seq);
                    }
                    if let Some(last) = expected.checked_sub(1) {
                        self.last_emitted_seq = Some(self.last_emitted_seq.map_or(last, |prev| prev.max(last)));
                    }
                    self.waiting_for_keyframe = true;
                    outcome.request_pli = true;
                    continue;
                }
                break;
            }

            let packets: Vec<Packet> = seqs
                .iter()
                .filter_map(|seq| self.buffer.remove(seq))
                .collect();
            let last_seq = *seqs.last().unwrap_or(&first_seq);
            self.last_emitted_seq = Some(last_seq);

            match self.assemble_frame(&packets) {
                Ok(Some(frame)) => {
                    if frame.is_keyframe {
                        self.waiting_for_keyframe = false;
                    }
                    if !self.waiting_for_keyframe {
                        outcome.frames.push(frame);
                    } else {
                        outcome.request_pli = true;
                    }
                }
                Ok(None) => {
                    // Parameter-set-only packet (e.g. H.264 STAP-A with SPS/PPS before IDR); keep state intact.
                }
                Err(()) => {
                    self.waiting_for_keyframe = true;
                    outcome.request_pli = true;
                }
            }
        }
    }

    fn assemble_frame(&mut self, packets: &[Packet]) -> Result<Option<AssembledVideoFrame>, ()> {
        let Some(first) = packets.first() else {
            return Err(());
        };
        let rtp_timestamp = first.header.timestamp;
        let sequence_number = first.header.sequence_number;
        let payload_type = first.header.payload_type;

        match self.codec {
            VideoCodec::Vp8 => assemble_vp8(packets, rtp_timestamp, sequence_number, payload_type),
            VideoCodec::Vp9 => assemble_vp9(packets, rtp_timestamp, sequence_number, payload_type),
            VideoCodec::H264 => self.assemble_h264(packets, rtp_timestamp, sequence_number, payload_type),
            VideoCodec::Av1 => assemble_av1(packets, rtp_timestamp, sequence_number, payload_type),
        }
    }

    fn assemble_h264(
        &mut self,
        packets: &[Packet],
        rtp_timestamp: u32,
        sequence_number: u16,
        payload_type: u8,
    ) -> Result<Option<AssembledVideoFrame>, ()> {
        let mut depacketizer = H264Packet::default();
        if !depacketizer.is_partition_head(&packets[0].payload) {
            return Err(());
        }

        let mut annex_b = Vec::new();
        for pkt in packets {
            let chunk = depacketizer.depacketize(&pkt.payload).map_err(|_| ())?;
            annex_b.extend_from_slice(&chunk);
        }
        if annex_b.is_empty() {
            return Err(());
        }

        let mut has_sps = false;
        let mut has_pps = false;
        let mut has_idr = false;
        let mut has_vcl = false;

        for nalu in iter_annex_b_nalus(&annex_b) {
            let Some(&header) = nalu.first() else {
                continue;
            };
            match header & 0x1F {
                1 => has_vcl = true,
                5 => {
                    has_idr = true;
                    has_vcl = true;
                }
                7 => {
                    has_sps = true;
                    self.cached_h264_sps = Some(nalu.to_vec());
                }
                8 => {
                    has_pps = true;
                    self.cached_h264_pps = Some(nalu.to_vec());
                }
                2..=4 | 19..=21 => has_vcl = true,
                _ => {}
            }
        }

        if !has_vcl {
            return Ok(None);
        }

        let mut data = Vec::with_capacity(annex_b.len() + 128);
        if has_idr {
            if !has_sps && let Some(sps) = &self.cached_h264_sps {
                data.extend_from_slice(&[0, 0, 0, 1]);
                data.extend_from_slice(sps);
            }
            if !has_pps && let Some(pps) = &self.cached_h264_pps {
                data.extend_from_slice(&[0, 0, 0, 1]);
                data.extend_from_slice(pps);
            }
        }
        data.extend_from_slice(&annex_b);

        Ok(Some(AssembledVideoFrame {
            data,
            rtp_timestamp,
            sequence_number,
            payload_type,
            is_keyframe: has_idr,
        }))
    }
}

fn assemble_vp8(
    packets: &[Packet],
    rtp_timestamp: u32,
    sequence_number: u16,
    payload_type: u8,
) -> Result<Option<AssembledVideoFrame>, ()> {
    let mut depacketizer = Vp8Packet::default();
    if !depacketizer.is_partition_head(&packets[0].payload) {
        return Err(());
    }
    let mut data = Vec::new();
    for pkt in packets {
        let chunk = depacketizer.depacketize(&pkt.payload).map_err(|_| ())?;
        data.extend_from_slice(&chunk);
    }
    if data.is_empty() {
        return Err(());
    }
    // RFC 6386 uncompressed data chunk: bit 0 of byte 0 is frame_type (0 = keyframe, 1 = inter).
    let is_keyframe = (data[0] & 0x01) == 0;
    Ok(Some(AssembledVideoFrame {
        data,
        rtp_timestamp,
        sequence_number,
        payload_type,
        is_keyframe,
    }))
}

fn assemble_vp9(
    packets: &[Packet],
    rtp_timestamp: u32,
    sequence_number: u16,
    payload_type: u8,
) -> Result<Option<AssembledVideoFrame>, ()> {
    let first_depacketizer = Vp9Packet::default();
    if !first_depacketizer.is_partition_head(&packets[0].payload) {
        return Err(());
    }

    let mut data = Vec::new();
    let mut is_keyframe = false;
    for (idx, pkt) in packets.iter().enumerate() {
        let mut depacketizer = Vp9Packet::default();
        let chunk = depacketizer.depacketize(&pkt.payload).map_err(|_| ())?;
        if idx == 0 {
            is_keyframe = !depacketizer.p;
        }
        data.extend_from_slice(&chunk);
    }
    if data.is_empty() {
        return Err(());
    }
    Ok(Some(AssembledVideoFrame {
        data,
        rtp_timestamp,
        sequence_number,
        payload_type,
        is_keyframe,
    }))
}

fn assemble_av1(
    packets: &[Packet],
    rtp_timestamp: u32,
    sequence_number: u16,
    payload_type: u8,
) -> Result<Option<AssembledVideoFrame>, ()> {
    // RTP payload format for AV1: https://aomediacodec.github.io/av1-rtp-spec/
    let Some(first_pkt) = packets.first() else {
        return Err(());
    };
    let Some(&first_aggr) = first_pkt.payload.first() else {
        return Err(());
    };
    // Z=1 on the first packet means it begins in the middle of an OBU from a lost earlier packet.
    if (first_aggr & 0x80) != 0 {
        return Err(());
    }

    let mut is_keyframe = (first_aggr & 0x08) != 0; // N bit
    let mut raw_obus: Vec<Vec<u8>> = Vec::new();

    for pkt in packets {
        let payload = &pkt.payload;
        if payload.is_empty() {
            return Err(());
        }
        let aggr = payload[0];
        let z = (aggr & 0x80) != 0;
        let y = (aggr & 0x40) != 0;
        let w = (aggr >> 4) & 0x03;
        if (aggr & 0x08) != 0 {
            is_keyframe = true;
        }

        let mut offset = 1usize;
        let mut elements: Vec<&[u8]> = Vec::new();

        if w == 0 {
            while offset < payload.len() {
                let (len, consumed) = read_leb128(&payload[offset..]).ok_or(())?;
                offset += consumed;
                let end = offset.checked_add(len).ok_or(())?;
                if end > payload.len() {
                    return Err(());
                }
                elements.push(&payload[offset..end]);
                offset = end;
            }
        } else {
            for idx in 0..w {
                if idx + 1 < w {
                    let (len, consumed) = read_leb128(&payload[offset..]).ok_or(())?;
                    offset += consumed;
                    let end = offset.checked_add(len).ok_or(())?;
                    if end > payload.len() {
                        return Err(());
                    }
                    elements.push(&payload[offset..end]);
                    offset = end;
                } else {
                    if offset > payload.len() {
                        return Err(());
                    }
                    elements.push(&payload[offset..]);
                }
            }
        }

        for (idx, elem) in elements.iter().enumerate() {
            if elem.is_empty() {
                continue;
            }
            if idx == 0 && z {
                let last_obu = raw_obus.last_mut().ok_or(())?;
                last_obu.extend_from_slice(elem);
            } else {
                raw_obus.push(elem.to_vec());
            }
        }

        // If Y=0 and this packet closed a fragment cleanly, nothing special is required;
        // if Y=1, the next packet in this frame continues the last OBU with Z=1.
        let _ = y;
    }

    if raw_obus.is_empty() {
        return Err(());
    }

    let mut out = Vec::new();
    for obu in raw_obus {
        let Some(&obu_header) = obu.first() else {
            continue;
        };
        let obu_type = (obu_header >> 3) & 0x0F;
        if obu_type == 1 {
            // OBU_SEQUENCE_HEADER
            is_keyframe = true;
        }
        let has_extension = (obu_header & 0x04) != 0;
        let has_size_field = (obu_header & 0x02) != 0;
        if has_size_field {
            out.extend_from_slice(&obu);
            continue;
        }
        let header_len = if has_extension { 2 } else { 1 };
        if obu.len() < header_len {
            return Err(());
        }
        // Rewrite with obu_has_size_field=1 followed by LEB128 payload size so FFmpeg/libdav1d accepts it cleanly.
        out.push(obu_header | 0x02);
        if has_extension {
            out.push(obu[1]);
        }
        write_leb128(&mut out, obu.len() - header_len);
        out.extend_from_slice(&obu[header_len..]);
    }

    if out.is_empty() {
        return Err(());
    }

    Ok(Some(AssembledVideoFrame {
        data: out,
        rtp_timestamp,
        sequence_number,
        payload_type,
        is_keyframe,
    }))
}

fn read_leb128(data: &[u8]) -> Option<(usize, usize)> {
    let mut value = 0usize;
    for (idx, &byte) in data.iter().take(8).enumerate() {
        value |= usize::from(byte & 0x7F) << (idx * 7);
        if (byte & 0x80) == 0 {
            return Some((value, idx + 1));
        }
    }
    None
}

fn write_leb128(out: &mut Vec<u8>, mut value: usize) {
    loop {
        let mut byte = (value & 0x7F) as u8;
        value >>= 7;
        if value != 0 {
            byte |= 0x80;
        }
        out.push(byte);
        if value == 0 {
            break;
        }
    }
}

fn iter_annex_b_nalus(data: &[u8]) -> Vec<&[u8]> {
    let mut nalus = Vec::new();
    let mut idx = 0usize;
    let mut current_start: Option<usize> = None;

    while idx < data.len() {
        let start_code_len = if idx + 4 <= data.len() && data[idx..idx + 4] == [0, 0, 0, 1] {
            4
        } else if idx + 3 <= data.len() && data[idx..idx + 3] == [0, 0, 1] {
            3
        } else {
            0
        };

        if start_code_len > 0 {
            if let Some(start) = current_start
                && start < idx
            {
                nalus.push(&data[start..idx]);
            }
            idx += start_code_len;
            current_start = Some(idx);
        } else {
            idx += 1;
        }
    }

    if let Some(start) = current_start
        && start < data.len()
    {
        nalus.push(&data[start..]);
    }
    nalus
}

#[cfg(test)]
mod tests {
    use bytes::Bytes;
    use webrtc::rtp::header::Header;

    use super::*;

    fn packet(seq: u16, ts: u32, marker: bool, pt: u8, payload: &[u8]) -> Packet {
        Packet {
            header: Header {
                version: 2,
                marker,
                payload_type: pt,
                sequence_number: seq,
                timestamp: ts,
                ssrc: 0x1234_5678,
                ..Default::default()
            },
            payload: Bytes::copy_from_slice(payload),
        }
    }

    #[test]
    fn vp8_single_and_out_of_order_reassembly() {
        let mut asm = VideoFrameAssembler::new(VideoCodec::Vp8);
        // Packet 1 (keyframe header byte 0x10 = S=1): uncompressed byte 0x00 => keyframe.
        let out = asm.push_packet(packet(10, 9000, true, 96, &[0x10, 0x00, 0xAA, 0xBB]));
        assert!(!out.request_pli);
        assert_eq!(out.frames.len(), 1);
        assert!(out.frames[0].is_keyframe);
        assert_eq!(out.frames[0].data, vec![0x00, 0xAA, 0xBB]);

        // Send seq 12 (end of P-frame) before seq 11 (start of P-frame).
        let out_ooo = asm.push_packet(packet(12, 12000, true, 96, &[0x00, 0x22, 0x33, 0x44]));
        assert!(out_ooo.frames.is_empty());
        assert!(!out_ooo.request_pli);

        let out_in_order = asm.push_packet(packet(11, 12000, false, 96, &[0x10, 0x01, 0x11, 0x99]));
        assert_eq!(out_in_order.frames.len(), 1);
        assert!(!out_in_order.frames[0].is_keyframe);
        assert_eq!(out_in_order.frames[0].data, vec![0x01, 0x11, 0x99, 0x22, 0x33, 0x44]);
    }

    #[test]
    fn vp8_gap_triggers_pli_once_window_expires() {
        let mut asm = VideoFrameAssembler::new(VideoCodec::Vp8);
        assert_eq!(
            asm.push_packet(packet(1, 3000, true, 96, &[0x10, 0x00, 0xAA, 0xBB]))
                .frames
                .len(),
            1
        );

        let mut saw_pli = false;
        // Skip seq 2 and push seqs 3..25.
        for s in 3u16..26 {
            let outcome = asm.push_packet(packet(s, u32::from(s) * 3000, true, 96, &[0x10, 0x01, 0x11, 0x22]));
            if outcome.request_pli {
                saw_pli = true;
            }
        }
        assert!(saw_pli);
    }

    #[test]
    fn vp9_keyframe_and_interframe_reassembly() {
        let mut asm = VideoFrameAssembler::new(VideoCodec::Vp9);
        // First byte: B=1, P=0 (keyframe) => 0x08.
        let kf_start = asm.push_packet(packet(1, 9000, false, 98, &[0x08, 0x82, 0x01]));
        assert!(kf_start.frames.is_empty());
        // Continuation packet: B=0, E=1 => 0x04.
        let kf_end = asm.push_packet(packet(2, 9000, true, 98, &[0x04, 0x02, 0x03]));
        assert_eq!(kf_end.frames.len(), 1);
        assert!(kf_end.frames[0].is_keyframe);
        assert_eq!(kf_end.frames[0].data, vec![0x82, 0x01, 0x02, 0x03]);

        // P-frame: P=1, B=1, E=1 => 0x4C.
        let p_frame = asm.push_packet(packet(3, 12000, true, 98, &[0x4C, 0x99, 0x88]));
        assert_eq!(p_frame.frames.len(), 1);
        assert!(!p_frame.frames[0].is_keyframe);
        assert_eq!(p_frame.frames[0].data, vec![0x99, 0x88]);
    }

    #[test]
    fn h264_stapa_parameter_sets_and_fua_idr_reassembly() {
        let mut asm = VideoFrameAssembler::new(VideoCodec::H264);
        // STAP-A containing SPS (0x67, len 3) + PPS (0x68, len 2).
        let stap_a = [24, 0, 3, 0x67, 0x42, 0x00, 0, 2, 0x68, 0xCE];
        let param_out = asm.push_packet(packet(1, 9000, true, 102, &stap_a));
        assert!(param_out.frames.is_empty());
        assert!(!param_out.request_pli);

        // FU-A IDR (NAL type 5, nri 3 => indicator 0x7C):
        // start packet: S=1, E=0, type=5 => 0x85; end packet: S=0, E=1, type=5 => 0x45.
        let fu_start = [0x7C, 0x85, 0x88, 0x84];
        let fu_end = [0x7C, 0x45, 0x21, 0xFF];
        assert!(asm.push_packet(packet(2, 9000, false, 102, &fu_start)).frames.is_empty());
        let idr_out = asm.push_packet(packet(3, 9000, true, 102, &fu_end));
        assert_eq!(idr_out.frames.len(), 1);
        let frame = &idr_out.frames[0];
        assert!(frame.is_keyframe);
        assert_eq!(
            frame.data,
            vec![
                0, 0, 0, 1, 0x67, 0x42, 0x00, // SPS
                0, 0, 0, 1, 0x68, 0xCE, // PPS
                0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21, 0xFF, // IDR
            ]
        );
    }

    #[test]
    fn av1_reassembles_obu_elements_with_size_fields() {
        let mut asm = VideoFrameAssembler::new(VideoCodec::Av1);
        // Aggr header: Z=0, Y=0, W=2 (2 OBUs), N=1 (keyframe) => 0x28.
        // OBU 1: Sequence header (type 1 => 0x08), no size bit; len=2.
        // OBU 2: Frame header/tile (type 6 => 0x30), no size bit.
        let pkt = [0x28, 0x02, 0x08, 0xAA, 0x30, 0xBB, 0xCC];
        let out = asm.push_packet(packet(1, 9000, true, 41, &pkt));
        assert_eq!(out.frames.len(), 1);
        assert!(out.frames[0].is_keyframe);
        assert_eq!(
            out.frames[0].data,
            vec![
                0x0A, 0x01, 0xAA, // Sequence header with has_size_field=1, size=1
                0x32, 0x02, 0xBB, 0xCC, // Frame OBU with has_size_field=1, size=2
            ]
        );
    }
}
