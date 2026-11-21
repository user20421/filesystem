package backend_test

import (
	"net/http"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/google/uuid"
)

func TestRealAuthLogoutIntegration(t *testing.T) {
	baseURL := os.Getenv("SECURE_IM_INTEGRATION_URL")
	if baseURL == "" {
		t.Skip("set SECURE_IM_INTEGRATION_URL to run against a live backend")
	}
	client := &integrationClient{t: t, baseURL: strings.TrimRight(baseURL, "/"), http: &http.Client{Timeout: 20 * time.Second}}
	suffix := strings.ReplaceAll(uuid.NewString()[:8], "-", "")
	account := client.register("logout_"+suffix, "Logout")
	other := client.register("other_"+suffix, "Other")
	secondSession := client.login(account.User.Username, "integration-password-123")

	client.requestJSON(http.MethodPost, "/api/v1/auth/logout", other.Tokens.AccessToken, map[string]any{"refresh_token": account.Tokens.RefreshToken}, http.StatusNoContent)
	body := client.requestJSON(http.MethodPost, "/api/v1/auth/refresh", "", map[string]any{"refresh_token": account.Tokens.RefreshToken}, http.StatusOK)
	var current integrationAuth
	decodeData(t, body, &current)

	client.requestJSON(http.MethodPost, "/api/v1/auth/logout", "", map[string]any{"refresh_token": current.Tokens.RefreshToken}, http.StatusUnauthorized)
	client.requestJSON(http.MethodPost, "/api/v1/auth/logout", current.Tokens.AccessToken, map[string]any{"refresh_token": current.Tokens.RefreshToken}, http.StatusNoContent)
	client.requestJSON(http.MethodPost, "/api/v1/auth/logout", current.Tokens.AccessToken, map[string]any{"refresh_token": current.Tokens.RefreshToken}, http.StatusNoContent)
	client.requestJSON(http.MethodPost, "/api/v1/auth/logout", current.Tokens.AccessToken, map[string]any{"refresh_token": strings.Repeat("x", 43)}, http.StatusNoContent)

	client.requestJSON(http.MethodPost, "/api/v1/auth/refresh", "", map[string]any{"refresh_token": current.Tokens.RefreshToken}, http.StatusUnauthorized)
	client.requestJSON(http.MethodGet, "/api/v1/users/me", current.Tokens.AccessToken, nil, http.StatusOK)
	body = client.requestJSON(http.MethodPost, "/api/v1/auth/refresh", "", map[string]any{"refresh_token": secondSession.Tokens.RefreshToken}, http.StatusOK)
	var refreshed integrationAuth
	decodeData(t, body, &refreshed)
	client.requestJSON(http.MethodPost, "/api/v1/auth/logout", secondSession.Tokens.AccessToken, map[string]any{"refresh_token": refreshed.Tokens.RefreshToken}, http.StatusNoContent)
	client.requestJSON(http.MethodPost, "/api/v1/auth/refresh", "", map[string]any{"refresh_token": refreshed.Tokens.RefreshToken}, http.StatusUnauthorized)
}
