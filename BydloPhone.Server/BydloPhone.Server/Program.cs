using System.Collections.Concurrent;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json.Serialization;
using Microsoft.OpenApi;
using Microsoft.Extensions.Caching.Memory;
using Swashbuckle.AspNetCore.SwaggerGen;

var builder = WebApplication.CreateBuilder(args);
builder.Services.AddOpenApi();
builder.Services.AddEndpointsApiExplorer();
builder.Services.AddSwaggerGen(options =>
{
    options.OperationFilter<SendVoiceOperationFilter>();
});
builder.Services.AddMemoryCache();
builder.Services.AddHttpClient("stt", client =>
{
    client.BaseAddress = new Uri(
        builder.Configuration["Services:Stt"]
        ?? Environment.GetEnvironmentVariable("STT_URL")
        ?? "http://localhost:8000");
});
builder.Services.AddHttpClient("tts", client =>
{
    client.BaseAddress = new Uri(
        builder.Configuration["Services:Tts"]
        ?? Environment.GetEnvironmentVariable("TTS_URL")
        ?? "http://localhost:8001");
});

var app = builder.Build();

app.UseSwagger();
app.UseSwaggerUI(options =>
{
    options.SwaggerEndpoint("/swagger/v1/swagger.json", "BydloPhone API v1");
});

app.MapOpenApi();
app.MapPost("/api/sendVoice", SendVoiceAsync)
    .WithName("SendVoice")
    .WithSummary("Upload a WAV file for STT -> TTS processing")
    .WithDescription("Accepts multipart/form-data with deviceId, voice/style and a WAV file. The generated answer is queued in memory for /api/getNext.");

app.MapGet("/api/getNext", GetNext)
    .WithName("GetNext")
    .WithSummary("Get the next synthesized WAV for a device");

app.MapGet("/health", () => Results.Ok(new { status = "ok" }))
    .WithName("Health");

app.Run();

static async Task<IResult> SendVoiceAsync(
    HttpRequest request,
    IHttpClientFactory httpClientFactory,
    IMemoryCache cache,
    ILoggerFactory loggerFactory,
    CancellationToken ct)
{
    if (!request.HasFormContentType)
        return Results.BadRequest(new { error = "Expected multipart/form-data" });

    var form = await request.ReadFormAsync(ct);

    var deviceId = form["deviceId"].ToString();
    var voice = form["voice"].ToString();
    var style = form["style"].ToString();
    var file = form.Files["file"];

    if (string.IsNullOrWhiteSpace(deviceId))
        return Results.BadRequest(new { error = "deviceId is required" });

    if (string.IsNullOrWhiteSpace(voice))
        voice = string.IsNullOrWhiteSpace(style) ? "bydlo" : style;

    if (file is null || file.Length == 0)
        return Results.BadRequest(new { error = "file is required" });

    await using var audioStream = new MemoryStream();
    await file.CopyToAsync(audioStream, ct);
    var audioBytes = audioStream.ToArray();

    var logger = loggerFactory.CreateLogger("VoicePipeline");

    _ = Task.Run(async () =>
    {
        try
        {
            var recognizedText = await TranscribeAsync(httpClientFactory, audioBytes, file.FileName);

            // TODO DeepSeek API:
            // 1. Send recognizedText to DeepSeek with the required prompt/personality.
            // 2. Receive transformed text from DeepSeek.
            // 3. Replace this passthrough assignment with the DeepSeek response.
            var transformedText = recognizedText;

            var answerWav = await SynthesizeAsync(httpClientFactory, transformedText, voice);
            EnqueueAnswer(cache, deviceId, answerWav);
        }
        catch (Exception ex)
        {
            logger.LogError(ex, "Voice pipeline failed for device {DeviceId}", deviceId);
        }
    }, CancellationToken.None);

    return Results.Json(new { status = "accepted" }, statusCode: StatusCodes.Status202Accepted);
}

static IResult GetNext(string deviceId, IMemoryCache cache)
{
    if (string.IsNullOrWhiteSpace(deviceId))
        return Results.BadRequest(new { error = "deviceId is required" });

    if (!cache.TryGetValue(CacheKey(deviceId), out ConcurrentQueue<byte[]>? queue))
        return Results.NoContent();

    if (queue is null || !queue.TryDequeue(out var audioBytes))
        return Results.NoContent();

    return Results.File(
        fileContents: audioBytes,
        contentType: "audio/wav",
        fileDownloadName: "answer.wav"
    );
}

static async Task<string> TranscribeAsync(
    IHttpClientFactory httpClientFactory,
    byte[] audioBytes,
    string fileName)
{
    using var form = new MultipartFormDataContent();
    using var fileContent = new ByteArrayContent(audioBytes);
    fileContent.Headers.ContentType = new MediaTypeHeaderValue("audio/wav");
    form.Add(fileContent, "file", string.IsNullOrWhiteSpace(fileName) ? "voice.wav" : fileName);

    var response = await httpClientFactory.CreateClient("stt").PostAsync("/transcribe", form);
    response.EnsureSuccessStatusCode();

    var result = await response.Content.ReadFromJsonAsync<TranscribeResponse>();
    return result?.Text ?? string.Empty;
}

static async Task<byte[]> SynthesizeAsync(
    IHttpClientFactory httpClientFactory,
    string text,
    string voice)
{
    var response = await httpClientFactory.CreateClient("tts").PostAsJsonAsync(
        "/synthesize",
        new SynthesizeRequest(text, voice));
    response.EnsureSuccessStatusCode();

    return await response.Content.ReadAsByteArrayAsync();
}

static void EnqueueAnswer(IMemoryCache cache, string deviceId, byte[] audioBytes)
{
    var queue = cache.GetOrCreate(CacheKey(deviceId), entry =>
    {
        entry.SlidingExpiration = TimeSpan.FromHours(1);
        return new ConcurrentQueue<byte[]>();
    });

    queue!.Enqueue(audioBytes);
}

static string CacheKey(string deviceId) => $"responses:{deviceId}";

internal sealed record TranscribeResponse(
    [property: JsonPropertyName("text")] string Text);

internal sealed record SynthesizeRequest(
    [property: JsonPropertyName("text")] string Text,
    [property: JsonPropertyName("voice")] string Voice);

internal sealed class SendVoiceOperationFilter : IOperationFilter
{
    public void Apply(OpenApiOperation operation, OperationFilterContext context)
    {
        if (context.ApiDescription.HttpMethod != HttpMethods.Post ||
            context.ApiDescription.RelativePath != "api/sendVoice")
        {
            return;
        }

        var multipartSchema = new OpenApiSchema
        {
            Type = JsonSchemaType.Object,
            Required = new HashSet<string> { "deviceId", "file" },
            Properties = new Dictionary<string, IOpenApiSchema>
            {
                ["deviceId"] = new OpenApiSchema
                {
                    Type = JsonSchemaType.String,
                    Description = "Speaker/client id used as the IMemoryCache queue key."
                },
                ["voice"] = new OpenApiSchema
                {
                    Type = JsonSchemaType.String,
                    Description = "TTS voice profile. Supported: bydlo, politic, teacher, robot, anime.",
                    Default = "bydlo"
                },
                ["style"] = new OpenApiSchema
                {
                    Type = JsonSchemaType.String,
                    Description = "Legacy alias for voice. Used only when voice is empty."
                },
                ["file"] = new OpenApiSchema
                {
                    Type = JsonSchemaType.String,
                    Format = "binary",
                    Description = "Input WAV file."
                }
            }
        };

        operation.RequestBody = new OpenApiRequestBody
        {
            Required = true,
            Content = new Dictionary<string, OpenApiMediaType>
            {
                ["multipart/form-data"] = new OpenApiMediaType
                {
                    Schema = multipartSchema
                }
            }
        };

        operation.Responses ??= new OpenApiResponses();
        operation.Responses.TryAdd("202", new OpenApiResponse
        {
            Description = "Voice processing accepted."
        });
        operation.Responses.TryAdd("400", new OpenApiResponse
        {
            Description = "Invalid multipart/form-data request."
        });
    }
}
