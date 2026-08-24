param(
    [string]$Action,
    [string]$ApiToken,
    [string]$BuildUrlOrDir,
    [string]$BuildName,
    [string]$WordPressUrl = "https://yoursite.com/wp-json/jenkins-deploy/v1",
    [string]$JenkinsToken
)

function Show-Usage {
    Write-Host "Website Upload Script"
    Write-Host "Usage:"
    Write-Host "  create [ApiToken] [BuildUrl] [BuildName] [WebsiteUrl] [JenkinsKey]"
    Write-Host "  files [ApiToken] [DirectoryToUpload] [BuildName] [WebsiteUrl]"
    Write-Host ""
    Write-Host "Examples:"
    Write-Host "  .\website-upload.ps1 create `"token123`" `"https://jenkins.example.com/job/MyApp/123/`" `"MyApp-v1.2.3`" `"11a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6`""
    Write-Host "  .\website-upload.ps1 files `"token123`" `"C:\Build\Package`" `"MyApp-v1.2.3`""
}

function Create-WebsiteRelease {
    param(
        [string]$ApiToken,
        [string]$BuildUrl,
        [string]$BuildName,
        [string]$JenkinsToken
    )
	Write-Host "Creating website release for build: $BuildName"
    Write-Host "API Token: $($ApiToken.Substring(0, [Math]::Min(4, $ApiToken.Length)))..." # Show first 4 chars only
    Write-Host "Build Name: $BuildName"
    Write-Host "WordPress URL: $WordPressUrl"
	Write-Host "BuildUrl: $BuildUrl"
	Write-Host "JenkinsToken: $($JenkinsToken.Substring(0, [Math]::Min(4, $JenkinsToken.Length)))..."

	$description = ""
	$JenkinsUser = "deploy"
	$apiUrl = $BuildUrl.TrimEnd('/') + '/api/xml?wrapper=changes'
	$credentials = "${JenkinsUser}:${JenkinsToken}"
	$encodedCredentials = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($credentials))
	$headers = @{ 
		'Authorization' = "Basic $encodedCredentials"
	}
    try {
        Write-Host "Fetching build info from: $apiUrl"
        $response = Invoke-RestMethod -Uri $apiUrl -Headers $headers
        
		if ($response -is [System.Xml.XmlDocument]) {
            $description = Parse-JenkinsChangeset -XmlResponse $response
        } else {
            [xml]$xmlDoc = $response
            $description = Parse-JenkinsChangeset -XmlResponse $xmlDoc
        }
    }
    catch {
        Write-Error "Failed to get Jenkins build info: $($_.Exception.Message)"
        return @{ Success = $false }
    }
        
    $headers = @{ 'X-Jenkins-Token' = $ApiToken }
    $formData = @{
        name = $BuildName
        description = $description
    }

    $createUrl = "$WordPressUrl/create"

    try {
        Write-Host "Sending GET request to create release..." -ForegroundColor Cyan
        Write-Host "URL: $createUrl" -ForegroundColor Gray
        
		$response = Invoke-RestMethod -Uri $createUrl -Method Post -Body $formData -Headers $headers

        if ($response.success) {
            Write-Host "  Release created successfully!" -ForegroundColor Green
            Write-Host "  Download ID: $($response.download_id)"
            return
        } else {
            Write-Error "  Failed to create release: $($response.message)"
            return
        }
    }
    catch {
        Write-Error "  Error creating release: $($_.Exception.Message)"
        
        # Show more details for debugging
        if ($_.Exception.Response) {
            Write-Host "HTTP Status: $($_.Exception.Response.StatusCode)" -ForegroundColor Red
            Write-Host "Response: $($_.Exception.Response)" -ForegroundColor Red
        }
        return
    }
}

function Upload-FilesToWebsite {
    param(
        [string]$ApiToken,
        [string]$Directory,
        [string]$BuildName
    )

    Write-Host "Uploading files from directory: $Directory"
    Write-Host "API Token: $($ApiToken.Substring(0, [Math]::Min(4, $ApiToken.Length)))..." # Show first 4 chars only
    Write-Host "Build Name: $BuildName"
    Write-Host "WordPress URL: $WordPressUrl"

    if (-not (Test-Path $Directory)) {
        Write-Error "Directory not found: $Directory"
        exit 1
    }

    $validExtensions = @('.zip')
    $files = Get-ChildItem -Path $Directory -File | Where-Object {
        $validExtensions -contains $_.Extension.ToLower()
    }

    if ($files.Count -eq 0) {
        Write-Warning "No valid files found in directory: $Directory"
        Write-Warning "Looking for files with extensions: $($validExtensions -join ', ')"
        return
    }

    Write-Host "Found $($files.Count) files to upload:"
    $files | ForEach-Object { Write-Host "  - $($_.Name) ($([math]::Round($_.Length / 1MB, 2)) MB)" }

    $uploadUrl = "$WordPressUrl/upload"
    $headers = @{ 'X-Jenkins-Token' = $ApiToken }

    foreach ($file in $files) {
        Write-Host "Uploading: $($file.Name)..."

        try {
            $fileBytes = [System.IO.File]::ReadAllBytes($file.FullName)

            $boundary = [System.Guid]::NewGuid().ToString()
            $LF = "`r`n"

            Write-Host "  File size: $($fileBytes.Length) bytes"
            Write-Host "  Boundary: $boundary"
            $bodyLines = @(
                "--$boundary",
                "Content-Disposition: form-data; name=`"file`"; filename=`"$($file.Name)`"",
                "Content-Type: application/octet-stream",
                "",
                [System.Text.Encoding]::GetEncoding("iso-8859-1").GetString($fileBytes),
                "--$boundary",
                "Content-Disposition: form-data; name=`"build_name`"",
                "",
                "$BuildName",
                "--$boundary"
            )

            $body = $bodyLines -join $LF
            $headers['Content-Type'] = "multipart/form-data; boundary=$boundary"

            Write-Host "  Making POST request to: $uploadUrl"
            $response = Invoke-RestMethod -Uri $uploadUrl -Method Post -Body $body -Headers $headers

            Write-Host "    Got response from WordPress:" -ForegroundColor Green
			Write-Host "Response: $($response | ConvertTo-Json -Depth 3)" -ForegroundColor Green

            if ($response.success) {
                Write-Host "    Upload successful"
                if ($response.download_id) {
                    Write-Host "  Download ID: $($response.download_id)"
                }
            } else {
                Write-Error "    Upload failed: $($response.message)"
            }

        } catch {
            Write-Error "  ✗ Upload failed: $($_.Exception.Message)"
        }
    }
    Write-Host "Upload process completed"
}

function Get-GitHubChangeAuthor {
    param(
        [string]$CommitId,
        [string]$PrNumber
    )

    $headers = @{ 'User-Agent' = 'nxemu-website-upload' }

    if ($PrNumber) {
        try {
            $pr = Invoke-RestMethod -Uri "https://api.github.com/repos/N3xoX1/nxemu/pulls/$PrNumber" -Headers $headers
            if ($pr.user.login) {
                return $pr.user.login
            }
        } catch {
            Write-Host "Could not fetch GitHub PR #$PrNumber : $($_.Exception.Message)"
        }
    }

    if ($CommitId) {
        try {
            $commit = Invoke-RestMethod -Uri "https://api.github.com/repos/N3xoX1/nxemu/commits/$CommitId" -Headers $headers
            if ($commit.author.login) {
                return $commit.author.login
            }
            if ($commit.commit.author.name) {
                return $commit.commit.author.name
            }
        } catch {
            Write-Host "Could not fetch GitHub commit $CommitId : $($_.Exception.Message)"
        }
    }

    return ""
}

function Parse-JenkinsChangeset {
    param(
        [System.Xml.XmlDocument]$XmlResponse
    )
    
    $ProductDescription = ""
    
    $changesets = $XmlResponse.SelectNodes("//changeset")
    
    if ($changesets.Count -eq 0) {
        $changesets = $XmlResponse.SelectNodes("//changeSet")
    }
    
    foreach ($changeset in $changesets) {
        foreach ($item in $changeset.SelectNodes(".//item")) {
            $commitId = ""
            $title = ""
            
            $commitIdNode = $item.SelectSingleNode(".//commitId")
            if ($commitIdNode) {
                $commitId = $commitIdNode.InnerText
            }

            $msgNode = $item.SelectSingleNode(".//msg")
            if ($msgNode -and $msgNode.InnerText.Trim().Length -gt 0) {
                $title = $msgNode.InnerText
            } else {
                $commentNode = $item.SelectSingleNode(".//comment")
                if ($commentNode) {
                    $title = ($commentNode.InnerText -split "`r?`n")[0]
                }
            }

            $title = $title -replace "\s+", " "
            $title = $title.Trim()

            if ($title.Length -eq 0 -or $commitId.Length -eq 0) {
                continue
            }

            $prNumber = ""
            if ($title -match '(?:pull request #|#)(\d+)') {
                $prNumber = $Matches[1]
            }
            $title = $title -replace '\s*\((?:pull request )?#\d+\)\s*$', ''
            $title = $title.Trim()

            $author = Get-GitHubChangeAuthor -CommitId $commitId -PrNumber $prNumber
            $line = "- $title"

            if ($prNumber -and $author) {
                $line += " (PR [#$prNumber](https://github.com/N3xoX1/nxemu/pull/$prNumber) from [$author](https://github.com/$author))"
            } elseif ($prNumber) {
                $line += " (PR [#$prNumber](https://github.com/N3xoX1/nxemu/pull/$prNumber))"
            } elseif ($author) {
                $line += " (from [$author](https://github.com/$author))"
            } else {
                $line += " (commit: [$commitId](https://github.com/N3xoX1/nxemu/commit/$commitId))"
            }

            $ProductDescription += "$line`r`n"
        }
    }
    
    if ($ProductDescription.Length -gt 0) {
        return $ProductDescription.TrimEnd()
    } else {
        return "No code changes"
    }
}

if ([string]::IsNullOrEmpty($Action) -or
    [string]::IsNullOrEmpty($ApiToken) -or
    [string]::IsNullOrEmpty($BuildUrlOrDir) -or
    [string]::IsNullOrEmpty($BuildName)) {
    Show-Usage
    exit 1
}

if ($Action -ne "create" -and $Action -ne "files") {
    Write-Host "Error: Action must be either 'create' or 'files'" -ForegroundColor Red
    Show-Usage
    exit 1
}

if ($Action -eq "create") {
    Create-WebsiteRelease -ApiToken $ApiToken -BuildUrl $BuildUrlOrDir -BuildName $BuildName -JenkinsToken $JenkinsToken
} elseif ($Action -eq "files") {
    Upload-FilesToWebsite -ApiToken $ApiToken -Directory $BuildUrlOrDir -BuildName $BuildName
} else { 
    Show-Usage
    exit 1
}