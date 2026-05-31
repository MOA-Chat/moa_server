# MOA Chat Server

실시간 채팅 앱 **MOA**의 백엔드 서버. C++ IOCP 기반 TCP 소켓 통신으로 메시지 라우팅, 파일 관리, 인증 등을 처리한다.

---

## 기술 스택

| 분류 | 기술 |
|------|------|
| Language | C++17 |
| I/O Model | Windows IOCP (I/O Completion Ports) |
| Protocol | Protocol Buffers (protobuf) |
| Database | MySQL 8.0 (mysqlx C++ Connector) |
| Cloud Storage | Cloudflare R2 (S3-compatible REST, 커스텀 도메인) |
| Push | Firebase Cloud Messaging HTTP v1 (OAuth2) |
| Auth | 세션 토큰, OAuth2 (Google/Apple) — Spring Boot에 검증 위임 |
| TLS | nginx stream proxy (Let's Encrypt) |
| Logging | spdlog v1.15.1 |
| HTTP Client | libcurl + OpenSSL |
| Build | Visual Studio 2022, MSVC |

---

## 주요 기능

**채팅**
- 그룹 실시간 메시지 (텍스트, 이미지, 동영상, 파일)
- 그룹 내 1:1 DM (groupdm: conv_id 형식)
- 오프라인 메시지 동기화 (로그인 시 미수신 메시지 S_MessageBatch 일괄 push)
- 읽음 확인 (unread count 실시간 감소)
- 메시지 답장 / 수정 / 삭제 (soft delete)
- 이모지 반응, 채팅방 공지, 그룹 투표

**인증**
- 소셜 로그인 전용 — Google, Apple OAuth (검증은 Spring Boot가 담당)
- 세션 토큰 + 레이트 리밋 (50 pkt/sec)
- 중복 로그인 시 이전 기기 강제 종료

**앨범**
- 그룹별 폴더 구조 앨범 + 사진 댓글
- 구독 플랜별 스토리지 쿼터 (1GB / 10GB / 50GB / 120GB)
- 원자적 쿼터 체크 (`TryAddGroupStorageAtomic`) + 초과 시 소프트딜리트

**위치 히스토리**
- 그룹 공유 장소(Place) + 방문기록(Visit)
- history_id 기반 크로스그룹 중복 제거

**구독 / 결제**
- 그룹 방장 단위 인앱결제 (Google Play / Apple IAP 서버 검증)
- 만료 → 14일 grace → 쿼터 초과 사진 소프트딜리트 → 하드딜리트 (6시간 배치)
- Spring Boot 웹훅으로 Apple/Google 자동갱신 이벤트 수신

**파일**
- Cloudflare R2 Signed URL 기반 업로드 (서버 대역폭 미경유)
- 채팅 파일 14일 보관, 앨범 사진 쿼터 기반 영구/소프트딜리트

**기타**
- FCM 푸시 알림 (오프라인 수신자, 만료 토큰 자동 정리)
- 초대 코드 3일 만료 + 자동 갱신
- 방장 이전 (TransferOwnership)

---

## 서버 아키텍처

```
Client (React Native, TCP)
  │  TLS (port 8443)
  ▼
nginx (stream proxy — TLS 종료)
  │  plain TCP (127.0.0.1:3000)
  ▼
┌─────────────────────────────────────────────────┐
│  C++ IOCP Server                                │
│                                                 │
│  PacketDispatcher                               │
│    │                                            │
│    ├── Service Layer                            │
│    │   Auth / Chat / File / Group               │
│    │   Album / Location / Payment               │
│    │   Notification / ExpirationBatchJob        │
│    │                                            │
│    ├── Repository Layer (MySQL 8.0)             │
│    │   User / Message / Group / Album           │
│    │   Location / Subscription / FcmToken       │
│    │                                            │
│    └── Cloud Layer                              │
│        Cloudflare R2 (Signed URL)               │
│        FCM (HTTP v1 OAuth2)                     │
│        Spring Boot (OAuth 검증 + 웹훅)           │
└─────────────────────────────────────────────────┘
```

### 패킷 처리 흐름

```
TCP 수신 → RecvBuffer 누적 → Length-Prefixed Framing (4B 헤더 + protobuf)
  → PacketDispatcher::DispatchPacket()
    → Version Check → Auth Token Check → Rate Limit Check
    → switch(payload_case) → Service 호출 → DB 조회/저장 → 응답 패킷 전송
```

### 스레딩 모델

| 스레드 | 역할 | 개수 |
|--------|------|------|
| Main | 콘솔 입력, 서버 제어 | 1 |
| IOCP Worker | 패킷 수신 / 처리 / 응답 | `hardware_concurrency()` |
| Heartbeat | 좀비 세션 감지 (10초 주기) | 1 |
| ExpirationBatch | 만료 파일 삭제 + 구독 처리 (6시간 주기) | 1 |

---

## 프로젝트 구조

```
cppServer/
├── ServerCore/            # IOCP 네트워킹 프레임워크
│   ├── IocpCore           # IOCP 핸들 관리, Dispatch 루프
│   ├── Session            # 소켓 세션 (비동기 Recv/Send)
│   ├── Listener           # AcceptEx 기반 연결 수락
│   ├── RecvBuffer         # 링 버퍼 (8KB)
│   └── SendBuffer         # 전송 큐
│
├── cppServer/             # 메인 애플리케이션
│   ├── mainServer.cpp     # 엔트리포인트
│   ├── ServerSession      # 인증, 레이트리밋, 하트비트
│   ├── PacketDispatcher   # protobuf 패킷 라우팅
│   ├── CoreGlobal         # 전역 서비스 관리
│   ├── Logger             # spdlog 래퍼
│   │
│   ├── Service/           # 비즈니스 로직
│   │   ├── AuthService           소셜 로그인 / 탈퇴 (Spring Boot OAuth 검증 연동)
│   │   ├── ChatService           메시지 전송, 답장/삭제/수정, 읽음확인, 반응, 투표
│   │   ├── FileService           파일 업로드 (용량 체크 + R2 Signed URL)
│   │   ├── GroupService          그룹 생성 / 참여 / 탈퇴 / 삭제 / 방장이전
│   │   ├── AlbumService          앨범 CRUD, 사진 댓글, 쿼터 관리
│   │   ├── LocationService       장소/방문기록 저장·수정·삭제
│   │   ├── PaymentService        IAP 영수증 검증 (Google/Apple), 구독 UPSERT
│   │   ├── NotificationService   FCM 토큰 관리 + 푸시 발송
│   │   ├── UserManager           세션 관리, 중복로그인, 타임아웃
│   │   └── ExpirationBatchJob    만료 파일/구독 처리 배치
│   │
│   ├── DB/                # 데이터 접근 계층
│   │   ├── DBManager             MySQL 연결 (thread_local 스레드별 세션)
│   │   ├── UserRepository        사용자 CRUD
│   │   ├── MessageRepository     메시지 저장/조회, 읽음상태, 반응
│   │   ├── GroupRepository       그룹/멤버 CRUD, 스토리지 원자적 쿼터
│   │   ├── AlbumRepository       앨범/사진/댓글, 소프트딜리트, 쿼터
│   │   ├── LocationRepository    장소/방문기록 CRUD
│   │   ├── SubscriptionRepository 구독 상태 CRUD
│   │   ├── FileRepository        파일 메타데이터, 만료 관리
│   │   └── PollRepository        투표 생성/참여/종료
│   │
│   └── Cloud/             # 외부 서비스 연동
│       ├── FcmClient             FCM HTTP v1 (OAuth2 JWT)
│       └── R2Client              Cloudflare R2 REST API (Signed URL)
│
├── Protobuf/
│   └── protocol.proto     # 메시지 타입 정의
│
└── database/
    └── *.sql              # 순차 마이그레이션 (01~27)
```

---

## DB 스키마

| 테이블 | 설명 |
|--------|------|
| `users` / `user_oauth_providers` | 사용자 + OAuth provider 연동 |
| `groups` / `group_members` | 그룹 + 멤버 (역할, 초대코드, 스토리지 쿼터) |
| `conversations` / `conversation_participants` | 대화방 (group / groupdm) |
| `messages` / `read_status` | 메시지 + 읽음 확인 |
| `albums` / `album_photos` / `photo_comments` | 앨범 시스템 |
| `location_places` / `location_visits` / `location_photos` | 위치 히스토리 |
| `subscriptions` / `subscription_plans` | 구독 플랜 (Free/Basic/Plus/Max) |
| `fcm_tokens` | FCM 푸시 토큰 (기기별) |
| `polls` / `poll_votes` | 투표 |
| `announcements` | 채팅방 공지 |
| `message_reactions` | 이모지 반응 |

---

## 부하 테스트 결과

실제 앱과 동일한 패킷 시퀀스(Login → 동기화 → Heartbeat + Chat + ReadReceipt)로 측정.

| 항목 | 100명 | 5,000명 | 10,000명 |
|------|-------|---------|----------|
| 접속/로그인 | 100% | 100% | 99.99% |
| 메시지 실패 | 0 | 0 | 0 |
| 응답 avg | 2.2ms | 15.2ms | 101ms |
| 응답 p99 | 4ms | 110ms | 526ms |
| 메시지 처리량 | 190 msg/s | 5,198 msg/s | **6,941 msg/s** |
| 피크 메모리 | 30MB | 497MB | 975MB |
| 정리 후 메모리 | 21MB | 24MB | 25MB |

- 10,000명: **115만건** 메시지, 실패 0, **6,941 msg/s**
- 세션 종료 후 메모리/핸들 정상 회수 확인

---

## 트러블슈팅

### 1. DB 세션 경쟁 상태

**증상**: 100명 동시 로그인 시 21% 실패, 메시지 100% 타임아웃

**원인**: `DBManager`가 `mysqlx::Session` 하나를 모든 IOCP 워커 스레드가 공유. 동시 쿼리 시 패킷 충돌.

**수정**: `thread_local` 세션으로 변경 — 각 워커 스레드가 독립 MySQL 연결 보유.

```cpp
mysqlx::Session& DBManager::GetSession() {
    thread_local std::unique_ptr<mysqlx::Session> tls_session;
    if (!tls_session)
        tls_session = std::make_unique<mysqlx::Session>(settings);
    return *tls_session;
}
```

| 항목 | Before | After |
|------|--------|-------|
| 로그인 성공률 | 79% | **100%** |
| 메시지 | 0 성공 / 8434 실패 | **10458 성공 / 0 실패** |
| 응답 avg | 157ms | **2.2ms** |

### 2. 세션 메모리/핸들 누수

**증상**: 5,000명 접속 후 전원 disconnect → 핸들/메모리 그대로 유지.

**원인**: 클라이언트가 TCP를 먼저 끊으면 `DisconnectEx()` 비동기 완료 통지가 안 와서 `ReleaseSession()`이 안 불림.

**수정**: `CleanupAndRelease()` — DisconnectEx 비동기 체인 대신 직접 정리.

```cpp
void Session::CleanupAndRelease() {
    if (_connected.exchange(false) == false) return;
    OnDisconnected();
    if (_socket != INVALID_SOCKET) {
        ::closesocket(_socket);
        _socket = INVALID_SOCKET;
    }
    if (auto service = _service.lock())
        service->ReleaseSession(GetSession());
}
```

| 항목 | Before | After |
|------|--------|-------|
| 5000명 후 핸들 | 5206 → 5206 (누수) | 5205 → **202** |
| 5000명 후 메모리 | 503MB → 503MB (누수) | 497MB → **24MB** |
