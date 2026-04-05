const root = document.querySelector("[data-markdown]");
const nav = document.querySelector("[data-article-nav]");

if (root) {
  const markedLib = window.marked;
  const prevKicker = root.dataset.prevKicker || "Previous";
  const nextKicker = root.dataset.nextKicker || "Next";
  const homeKicker = root.dataset.homeKicker || "Archive";
  const homeLabel = root.dataset.homeLabel || "Back to index";
  const homeHref = root.dataset.homeHref || "../index.html";
  const loadingMessage = root.dataset.loading || "Loading article…";
  const fetchError =
    root.dataset.fetchError ||
    "This page needs to be served by GitHub Pages or a local web server so it can load the Markdown source.";
  const markedError =
    root.dataset.markedError ||
    "Marked failed to load. Serve this repository over HTTP and reload the page.";

  const setExternalLinkBehavior = () => {
    for (const link of root.querySelectorAll("a[href]")) {
      const url = new URL(link.getAttribute("href"), window.location.href);
      if (url.origin !== window.location.origin) {
        link.target = "_blank";
        link.rel = "noreferrer noopener";
      }
    }
  };

  const renderNav = () => {
    if (!nav) {
      return;
    }

    const cards = [];
    const prevHref = root.dataset.prevHref;
    const prevLabel = root.dataset.prevLabel;
    const nextHref = root.dataset.nextHref;
    const nextLabel = root.dataset.nextLabel;

    if (prevHref && prevLabel) {
      cards.push(
        `<a class="nav-card" href="${prevHref}"><span>${prevKicker}</span><strong>${prevLabel}</strong></a>`
      );
    } else {
      cards.push("<div></div>");
    }

    cards.push(
      `<a class="nav-card nav-card-home" href="${homeHref}"><span>${homeKicker}</span><strong>${homeLabel}</strong></a>`
    );

    if (nextHref && nextLabel) {
      cards.push(
        `<a class="nav-card" href="${nextHref}"><span>${nextKicker}</span><strong>${nextLabel}</strong></a>`
      );
    } else {
      cards.push("<div></div>");
    }

    nav.innerHTML = cards.join("");
  };

  const renderError = (message) => {
    root.innerHTML = `<p class="article-error">${message}</p>`;
  };

  const loadArticle = async () => {
    if (!markedLib) {
      renderError(markedError);
      return;
    }

    root.innerHTML = `<p class="article-loading">${loadingMessage}</p>`;

    try {
      const response = await fetch(root.dataset.markdown);
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }

      const source = await response.text();
      markedLib.setOptions({
        gfm: true,
        breaks: false,
      });
      root.innerHTML = markedLib.parse(source);
      setExternalLinkBehavior();
      renderNav();
    } catch (error) {
      renderError(fetchError);
      console.error(error);
    }
  };

  loadArticle();
}
