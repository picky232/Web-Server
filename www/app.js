// 브라우저 쪽 데모 스크립트: /api/* 요청은 C 서버가 백엔드로 중계한다.
const result = document.getElementById("result");

async function show(promise) {
  try {
    const res = await promise;
    const text = await res.text();
    result.textContent = `HTTP ${res.status} ${res.statusText}\n` +
      `Content-Type: ${res.headers.get("content-type")}\n\n${text}`;
  } catch (err) {
    result.textContent = `요청 실패: ${err}`;
  }
}

document.getElementById("btn-hello").addEventListener("click", () => {
  show(fetch("/api/hello"));
});

document.getElementById("btn-time").addEventListener("click", () => {
  show(fetch("/api/time"));
});

document.getElementById("echo-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const body = document.getElementById("echo-body").value;
  show(fetch("/api/echo", {
    method: "POST",
    headers: { "Content-Type": "text/plain; charset=utf-8" },
    body,
  }));
});
