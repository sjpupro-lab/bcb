# BCB Benchmarks

재현: `make bench` (기본 코퍼스: `tests/corpus/pride_and_prejudice.txt`).
직접 지정: `build/bcb-bench <corpus.txt> [excerpt_offset] [excerpt_len]`.

## 측정 방법

- 코퍼스의 한 발췌(기본 4096 B)를 대상으로 압축비를 측정한다.
- BCB 는 발췌 **직전** 의 코퍼스 구간으로 학습한다 (학습량 sweep: 0 / 50 / 200 / 500 KB).
  인코더·디코더가 동일 코퍼스를 공유한다는 전제를 그대로 반영한다.
- 표준 압축기(gzip/bzip2/xz/zstd)는 발췌 단독(공유 모델 없음)을 압축한다.
- 모든 BCB 측정은 round-trip 무손실 검증(`lossless: yes`)을 동반한다.

## Pride and Prejudice — 4KB 발췌 (offset 600000)

이 레포의 `make bench` 실측값 (Gutenberg #1342, 772389 B):

| 학습량 / train | BCB       | lossless |
|----------------|-----------|----------|
| 0 KB           | 1.96×     | yes      |
| 50 KB          | 2.78×     | yes      |
| 200 KB         | 3.04×     | yes      |
| 500 KB         | **3.16×** | yes      |

| 표준 압축기 (발췌 단독) | ratio |
|--------------------------|-------|
| gzip-9                   | 2.05× |
| bzip2-9                  | 2.19× |
| xz-9                     | 1.99× |

BCB 는 50 KB 학습만으로 이미 bzip2-9 를 넘어서고, 500 KB 학습에서 3.16× 로
세 압축기를 모두 능가한다. 발췌 위치·코퍼스 판본에 따라 수치는 소폭 달라질 수 있다.

### 원 레퍼런스 (다른 발췌)

원 프로토타입 측정값(참고): 50KB 2.59× / 200KB 2.96× / 500KB 3.13×,
같은 발췌에서 gzip-9 2.12× · bz2 2.21× · lzma-9 2.02×.

## 코퍼스

`tests/corpus/` 에 Project Gutenberg 4권:

- `pride_and_prejudice.txt` (#1342)
- `frankenstein.txt` (#84)
- `alice_in_wonderland.txt` (#11)
- `moby_dick.txt` (#2701)

## 해석

학습량이 늘수록 BCB 의 학습된 prior P 가 데이터 분포에 가까워져 cross-entropy 가 낮아진다.
표준 압축기는 공유 모델이 없어 4KB 단독에서 사전 학습 이점을 얻지 못한다 —
이것이 BCB 의 설계 의도(공유 prior)가 드러나는 지점이다.
